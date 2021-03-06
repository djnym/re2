// Copyright (c) 2010-2018 Tuncer Ayaz. All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <erl_nif.h>

#include <re2/re2.h>
#include <map>
#include <vector>
#include <memory>

#ifdef DEBUG
#include <iostream>
#define DBG(M)                                                                \
    do {                                                                      \
        std::cerr << "re2: " << M;                                            \
    } while (false)
#else
#define DBG(M)                                                                \
    do {                                                                      \
    } while (false)
#endif

namespace {
struct compileoptions
{
    re2::RE2::Options re2opts;
};

struct matchoptions
{
    enum value_spec
    {
        VS_ALL,
        VS_ALL_BUT_FIRST,
        VS_FIRST,
        VS_NONE,
        VS_VLIST
    };
    enum capture_type
    {
        CT_INDEX,
        CT_LIST,
        CT_BINARY
    };

    bool caseless;
    int offset;
    value_spec vs;
    capture_type ct;
    ERL_NIF_TERM vlist;

    matchoptions(ErlNifEnv* env)
    : caseless(false)
    , offset(0)
    , vs(VS_ALL)
    , ct(CT_BINARY)
    {
        vlist = enif_make_list(env, 0);
    }
};

struct replaceoptions
{
    bool global;
    replaceoptions()
    : global(false)
    {}
};

// Cleanup function for C++ object created with enif allocator via C++
// placement syntax which necessitates explicit invocation of the object's
// destructor. This is used in the NIF resource cleanup callback and in a
// unique_ptr's deleter.
template <typename T>
void cleanup_obj_ptr(T*& ptr)
{
    if (ptr != nullptr) {
        ptr->~T();
        enif_free(ptr);
        ptr = nullptr;
    }
}

// Deleter for use with enif allocated C++ object owned by a unique_ptr.
template <typename T>
struct EnifDeleter
{
    void operator()(T* ptr) { cleanup_obj_ptr(ptr); }
};
using Re2UniquePtr = std::unique_ptr<re2::RE2, EnifDeleter<re2::RE2>>;
}  // namespace

struct re2_handle
{
    // RE2 objects are thread safe. no locking required.
    re2::RE2* re;
};

//
// Use a union for pointer type conversion to avoid compiler warnings
// about strict-aliasing violations with gcc-4.1. gcc >= 4.2 does not
// emit the warning.
// TODO: Reconsider use of union once gcc-4.1 is obsolete?
//
union re2_handle_union
{
    void* vp;
    re2_handle* p;
};

#if ERL_NIF_MAJOR_VERSION > 2                                                 \
    || (ERL_NIF_MAJOR_VERSION == 2 && ERL_NIF_MINOR_VERSION >= 7)
#define NIF_FUNC_ENTRY(name, arity, fun)                                      \
    {                                                                         \
        name, arity, fun, 0                                                   \
    }
#else
#define NIF_FUNC_ENTRY(name, arity, fun)                                      \
    {                                                                         \
        name, arity, fun                                                      \
    }
#endif

#if ERL_NIF_MAJOR_VERSION > 2
#define RE2_HAVE_DIRTY_SCHEDULERS 1
#elif ERL_NIF_MAJOR_VERSION == 2
#if ERL_NIF_MINOR_VERSION >= 7 && ERL_NIF_MINOR_VERSION < 11
#ifdef ERL_NIF_DIRTY_SCHEDULER_SUPPORT
#define RE2_HAVE_DIRTY_SCHEDULERS 1
#endif
#elif ERL_NIF_MINOR_VERSION >= 11
#define RE2_HAVE_DIRTY_SCHEDULERS 1
#else
#undef RE2_HAVE_DIRTY_SCHEDULERS
#endif
#endif

#ifdef RE2_HAVE_DIRTY_SCHEDULERS

static bool have_online_dirty_schedulers()
{
    ErlNifSysInfo si;
    enif_system_info(&si, sizeof(si));
    DBG("dirty_scheduler_support: " << si.dirty_scheduler_support << "\n");
    return si.dirty_scheduler_support != 0;
}

#define DS_MODE ERL_NIF_DIRTY_JOB_CPU_BOUND
#define SCHEDULE_NIF enif_schedule_nif

#else

static bool have_online_dirty_schedulers()
{
    return false;
}

#define DS_MODE 0
static ERL_NIF_TERM SCHEDULE_NIF(
    ErlNifEnv* env,
    const char*,  // fun_name
    int,          // flags
    ERL_NIF_TERM (*fp)(ErlNifEnv*, int, const ERL_NIF_TERM[]),
    int argc,
    const ERL_NIF_TERM argv[])
{
    return (*fp)(env, argc, argv);
}
#endif

// static variables
static int ds_flags                          = 0;
static ErlNifResourceType* re2_resource_type = nullptr;
static ERL_NIF_TERM a_ok;
static ERL_NIF_TERM a_error;
static ERL_NIF_TERM a_match;
static ERL_NIF_TERM a_nomatch;
static ERL_NIF_TERM a_capture;
static ERL_NIF_TERM a_global;
static ERL_NIF_TERM a_offset;
static ERL_NIF_TERM a_all;
static ERL_NIF_TERM a_all_but_first;
static ERL_NIF_TERM a_first;
static ERL_NIF_TERM a_none;
static ERL_NIF_TERM a_index;
static ERL_NIF_TERM a_binary;
static ERL_NIF_TERM a_caseless;
static ERL_NIF_TERM a_max_mem;
static ERL_NIF_TERM a_err_enif_alloc_binary;
static ERL_NIF_TERM a_err_enif_alloc_resource;
static ERL_NIF_TERM a_err_enif_alloc;
static ERL_NIF_TERM a_err_enif_get_atom;
static ERL_NIF_TERM a_err_enif_get_string;
static ERL_NIF_TERM a_re2_NoError;
static ERL_NIF_TERM a_re2_ErrorInternal;
static ERL_NIF_TERM a_re2_ErrorBadEscape;
static ERL_NIF_TERM a_re2_ErrorBadCharClass;
static ERL_NIF_TERM a_re2_ErrorBadCharRange;
static ERL_NIF_TERM a_re2_ErrorMissingBracket;
static ERL_NIF_TERM a_re2_ErrorMissingParen;
static ERL_NIF_TERM a_re2_ErrorTrailingBackslash;
static ERL_NIF_TERM a_re2_ErrorRepeatArgument;
static ERL_NIF_TERM a_re2_ErrorRepeatSize;
static ERL_NIF_TERM a_re2_ErrorRepeatOp;
static ERL_NIF_TERM a_re2_ErrorBadPerlOp;
static ERL_NIF_TERM a_re2_ErrorBadUTF8;
static ERL_NIF_TERM a_re2_ErrorBadNamedCapture;
static ERL_NIF_TERM a_re2_ErrorPatternTooLarge;

static void init_atoms(ErlNifEnv* env)
{
    a_ok                         = enif_make_atom(env, "ok");
    a_error                      = enif_make_atom(env, "error");
    a_match                      = enif_make_atom(env, "match");
    a_nomatch                    = enif_make_atom(env, "nomatch");
    a_capture                    = enif_make_atom(env, "capture");
    a_global                     = enif_make_atom(env, "global");
    a_offset                     = enif_make_atom(env, "offset");
    a_all                        = enif_make_atom(env, "all");
    a_all_but_first              = enif_make_atom(env, "all_but_first");
    a_first                      = enif_make_atom(env, "first");
    a_none                       = enif_make_atom(env, "none");
    a_index                      = enif_make_atom(env, "index");
    a_binary                     = enif_make_atom(env, "binary");
    a_caseless                   = enif_make_atom(env, "caseless");
    a_max_mem                    = enif_make_atom(env, "max_mem");
    a_err_enif_alloc_binary      = enif_make_atom(env, "enif_alloc_binary");
    a_err_enif_alloc_resource    = enif_make_atom(env, "enif_alloc_resource");
    a_err_enif_alloc             = enif_make_atom(env, "enif_alloc");
    a_err_enif_get_atom          = enif_make_atom(env, "enif_get_atom");
    a_err_enif_get_string        = enif_make_atom(env, "enif_get_string");
    a_re2_NoError                = enif_make_atom(env, "no_error");
    a_re2_ErrorInternal          = enif_make_atom(env, "internal");
    a_re2_ErrorBadEscape         = enif_make_atom(env, "bad_escape");
    a_re2_ErrorBadCharClass      = enif_make_atom(env, "bad_char_class");
    a_re2_ErrorBadCharRange      = enif_make_atom(env, "bad_char_range");
    a_re2_ErrorMissingBracket    = enif_make_atom(env, "missing_bracket");
    a_re2_ErrorMissingParen      = enif_make_atom(env, "missing_paren");
    a_re2_ErrorTrailingBackslash = enif_make_atom(env, "trailing_backslash");
    a_re2_ErrorRepeatArgument    = enif_make_atom(env, "repeat_argument");
    a_re2_ErrorRepeatSize        = enif_make_atom(env, "repeat_size");
    a_re2_ErrorRepeatOp          = enif_make_atom(env, "repeat_op");
    a_re2_ErrorBadPerlOp         = enif_make_atom(env, "bad_perl_op");
    a_re2_ErrorBadUTF8           = enif_make_atom(env, "bad_utf8");
    a_re2_ErrorBadNamedCapture   = enif_make_atom(env, "bad_named_capture");
    a_re2_ErrorPatternTooLarge   = enif_make_atom(env, "pattern_too_large");
}

static void cleanup_handle(re2_handle* handle)
{
    cleanup_obj_ptr(handle->re);
}

//
// Make an error tuple
//
static ERL_NIF_TERM error(ErlNifEnv* env, const ERL_NIF_TERM err)
{
    return enif_make_tuple2(env, a_error, err);
}

//
// convert RE2 error code to error term
//
static ERL_NIF_TERM re2error(ErlNifEnv* env, const re2::RE2& re)
{
    ERL_NIF_TERM code;

    switch (re.error_code()) {
    case re2::RE2::ErrorInternal:  // Unexpected error
        code = a_re2_ErrorInternal;
        break;
    // Parse errors
    case re2::RE2::ErrorBadEscape:  // bad escape sequence
        code = a_re2_ErrorBadEscape;
        break;
    case re2::RE2::ErrorBadCharClass:  // bad character class
        code = a_re2_ErrorBadCharClass;
        break;
    case re2::RE2::ErrorBadCharRange:  // bad character class range
        code = a_re2_ErrorBadCharRange;
        break;
    case re2::RE2::ErrorMissingBracket:  // missing closing ]
        code = a_re2_ErrorMissingBracket;
        break;
    case re2::RE2::ErrorMissingParen:  // missing closing )
        code = a_re2_ErrorMissingParen;
        break;
    case re2::RE2::ErrorTrailingBackslash:  // trailing \ at end of regexp
        code = a_re2_ErrorTrailingBackslash;
        break;
    case re2::RE2::ErrorRepeatArgument:  // repeat argument missing, e.g. "*"
        code = a_re2_ErrorRepeatArgument;
        break;
    case re2::RE2::ErrorRepeatSize:  // bad repetition argument
        code = a_re2_ErrorRepeatSize;
        break;
    case re2::RE2::ErrorRepeatOp:  // bad repetition operator
        code = a_re2_ErrorRepeatOp;
        break;
    case re2::RE2::ErrorBadPerlOp:  // bad perl operator
        code = a_re2_ErrorBadPerlOp;
        break;
    case re2::RE2::ErrorBadUTF8:  // invalid UTF-8 in regexp
        code = a_re2_ErrorBadUTF8;
        break;
    case re2::RE2::ErrorBadNamedCapture:  // bad named capture group
        code = a_re2_ErrorBadNamedCapture;
        break;
    case re2::RE2::ErrorPatternTooLarge:  // pattern too large (compile failed)
        code = a_re2_ErrorPatternTooLarge;
        break;
    default:
    case re2::RE2::NoError:
        code = a_re2_NoError;
        break;
    }

    ERL_NIF_TERM error
        = enif_make_string(env, re.error().c_str(), ERL_NIF_LATIN1);
    ERL_NIF_TERM error_arg
        = enif_make_string(env, re.error_arg().c_str(), ERL_NIF_LATIN1);

    return enif_make_tuple2(
        env, a_error, enif_make_tuple3(env, code, error, error_arg));
}

static char* alloc_atom(ErlNifEnv* env, const ERL_NIF_TERM atom, unsigned* len)
{
    unsigned atom_len;
    if (!enif_get_atom_length(env, atom, &atom_len, ERL_NIF_LATIN1))
        return nullptr;
    atom_len++;
    *len = atom_len;
    return (char*)enif_alloc(atom_len);
}

static char* alloc_str(ErlNifEnv* env, const ERL_NIF_TERM list, unsigned* len)
{
    unsigned list_len;
    if (!enif_get_list_length(env, list, &list_len))
        return nullptr;
    list_len++;
    *len = list_len;
    return (char*)enif_alloc(list_len);
}

// ===========
// re2:compile
// ===========

//
// Options = [ Option ]
// Option = caseless | {max_mem, int()}
//
static bool parse_compile_options(
    ErlNifEnv* env, const ERL_NIF_TERM list, re2::RE2::Options& opts)
{
    if (enif_is_empty_list(env, list))
        return true;

    ERL_NIF_TERM L, H, T;

    for (L = list; enif_get_list_cell(env, L, &H, &T); L = T) {
        const ERL_NIF_TERM* tuple;
        int tuplearity = -1;

        if (enif_is_identical(H, a_caseless)) {

            // caseless

            opts.set_case_sensitive(false);
        } else if (enif_get_tuple(env, H, &tuplearity, &tuple)) {

            if (tuplearity == 2) {

                if (enif_is_identical(tuple[0], a_max_mem)) {

                    // {max_mem, int()}

                    int max_mem = 0;
                    if (enif_get_int(env, tuple[1], &max_mem))
                        opts.set_max_mem(max_mem);
                    else
                        return false;
                }
            }
        } else {
            return false;
        }
    }

    return true;
}

static ERL_NIF_TERM re2_compile_impl(
    ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary pdata;

    if (enif_inspect_iolist_as_binary(env, argv[0], &pdata)) {
        const re2::StringPiece p((const char*)pdata.data, pdata.size);
        re2_handle* handle = (re2_handle*)enif_alloc_resource(
            re2_resource_type, sizeof(re2_handle));

        if (handle == nullptr) {
            return error(env, a_err_enif_alloc_resource);
        }

        handle->re = nullptr;

        re2::RE2::Options re2opts;
        re2opts.set_log_errors(false);

        if (argc == 2 && !parse_compile_options(env, argv[1], re2opts)) {
            cleanup_handle(handle);
            enif_release_resource(handle);
            return enif_make_badarg(env);
        }

        re2::RE2* re2 = (re2::RE2*)enif_alloc(sizeof(re2::RE2));
        if (re2 == nullptr) {
            cleanup_handle(handle);
            enif_release_resource(handle);
            return error(env, a_err_enif_alloc);
        }
        handle->re = new (re2) re2::RE2(p, re2opts);  // placement new

        if (!handle->re->ok()) {
            ERL_NIF_TERM error = re2error(env, *(handle->re));
            cleanup_handle(handle);
            enif_release_resource(handle);
            return error;
        }

        ERL_NIF_TERM result = enif_make_resource(env, handle);
        enif_release_resource(handle);
        return enif_make_tuple2(env, a_ok, result);
    } else {
        return enif_make_badarg(env);
    }
}

// =========
// re2:match
// =========

static void parse_match_capture_options(
    ErlNifEnv* env,
    matchoptions& opts,
    const ERL_NIF_TERM* tuple,
    int tuplearity)
{
    bool vs_set = false;
    if (enif_is_atom(env, tuple[1])) {

        // ValueSpec = all | all_but_first | first | none

        if (enif_is_atom(env, tuple[1]) > 0) {

            if (enif_is_identical(tuple[1], a_all))
                opts.vs = matchoptions::VS_ALL;
            else if (enif_is_identical(tuple[1], a_all_but_first))
                opts.vs = matchoptions::VS_ALL_BUT_FIRST;
            else if (enif_is_identical(tuple[1], a_first))
                opts.vs = matchoptions::VS_FIRST;
            else if (enif_is_identical(tuple[1], a_none))
                opts.vs = matchoptions::VS_NONE;

            vs_set = true;
        }
    } else if (!enif_is_empty_list(env, tuple[1])) {

        // ValueSpec = ValueList
        // ValueList = [ ValueID ]
        // ValueID = int() | string() | atom()

        opts.vlist = tuple[1];
        vs_set     = true;
        opts.vs    = matchoptions::VS_VLIST;
    }

    // Type = index | binary

    if (tuplearity == 3 && vs_set) {

        if (enif_is_identical(tuple[2], a_index))
            opts.ct = matchoptions::CT_INDEX;
        else if (enif_is_identical(tuple[2], a_binary))
            opts.ct = matchoptions::CT_BINARY;
    }
}

//
// Options = [ Option ]
// Option = caseless | {offset, non_neg_integer()}
//          | {capture,ValueSpec} | {capture,ValueSpec,Type}
// Type = index | binary
// ValueSpec = all | all_but_first | first | none | ValueList
// ValueList = [ ValueID ]
// ValueID = int() | string() | atom()
//
static bool parse_match_options(
    ErlNifEnv* env, const ERL_NIF_TERM list, matchoptions& opts)
{
    if (enif_is_empty_list(env, list))
        return true;

    ERL_NIF_TERM L, H, T;

    for (L = list; enif_get_list_cell(env, L, &H, &T); L = T) {
        const ERL_NIF_TERM* tuple;
        int tuplearity = -1;

        if (enif_is_identical(H, a_caseless)) {

            // caseless

            opts.caseless = true;
        } else if (enif_get_tuple(env, H, &tuplearity, &tuple)) {

            if (tuplearity == 2 || tuplearity == 3) {

                // {offset,N} or {capture,ValueSpec}

                if (enif_is_identical(tuple[0], a_offset)) {

                    // {offset, int()}

                    int offset = 0;
                    if (enif_get_int(env, tuple[1], &offset)) {
                        opts.offset = offset;
                    } else {
                        return false;
                    }
                } else if (enif_is_identical(tuple[0], a_capture)) {

                    // {capture,ValueSpec,Type}
                    parse_match_capture_options(env, opts, tuple, tuplearity);
                }
            }
        } else {
            return false;
        }
    }

    return true;
}

//
// build result for re2:match
//
static ERL_NIF_TERM mres(
    ErlNifEnv* env,
    const re2::StringPiece& str,
    const re2::StringPiece& match,
    const matchoptions::capture_type ct)
{
    switch (ct) {
    case matchoptions::CT_BINARY:
        ErlNifBinary bmatch;
        if (!enif_alloc_binary(match.size(), &bmatch))
            return a_err_enif_alloc_binary;
        memcpy(bmatch.data, match.data(), match.size());
        return enif_make_binary(env, &bmatch);
    default:
    case matchoptions::CT_INDEX:
        int l, r;
        if (match.empty()) {
            l = -1;
            r = 0;
        } else {
            l = match.data() - str.data();
            r = match.size();
        }
        return enif_make_tuple2(
            env, enif_make_int(env, l), enif_make_int(env, r));
    }
}

static ERL_NIF_TERM re2_match_ret_vlist(
    ErlNifEnv* env,
    const re2::RE2& re,
    const re2::StringPiece& s,
    const matchoptions& opts,
    std::vector<re2::StringPiece>& group,
    int n)
{
    std::vector<ERL_NIF_TERM> vec;
    const auto& nmap = re.NamedCapturingGroups();
    ERL_NIF_TERM VL, VH, VT;

    // empty StringPiece for unfound ValueIds
    const re2::StringPiece empty;

    for (VL = opts.vlist; enif_get_list_cell(env, VL, &VH, &VT); VL = VT) {
        int nid = 0;

        if (enif_get_int(env, VH, &nid) && nid > 0) {

            // ValueID int()

            if (nid < n) {
                const re2::StringPiece match = group[nid];
                ERL_NIF_TERM res;
                if (!match.empty())
                    res = mres(env, s, group[nid], opts.ct);
                else
                    res = mres(env, s, empty, opts.ct);

                if (enif_is_identical(res, a_err_enif_alloc_binary))
                    return error(env, a_err_enif_alloc_binary);
                else
                    vec.push_back(res);
            } else {
                vec.push_back(mres(env, s, empty, opts.ct));
            }
        } else if (enif_is_atom(env, VH)) {

            // ValueID atom()

            unsigned atom_len;
            char* a_id = alloc_atom(env, VH, &atom_len);
            if (a_id == nullptr)
                return error(env, a_err_enif_alloc);

            if (enif_get_atom(env, VH, a_id, atom_len, ERL_NIF_LATIN1) > 0) {
                auto it = nmap.find(a_id);

                ERL_NIF_TERM res;
                if (it != nmap.end())
                    res = mres(env, s, group[it->second], opts.ct);
                else
                    res = mres(env, s, empty, opts.ct);

                if (enif_is_identical(res, a_err_enif_alloc_binary)) {
                    enif_free(a_id);
                    return error(env, a_err_enif_alloc_binary);
                } else {
                    vec.push_back(res);
                }
            } else {
                enif_free(a_id);
                return error(env, a_err_enif_get_atom);
            }

            enif_free(a_id);
        } else {

            // ValueID string()

            unsigned str_len;
            char* str_id = alloc_str(env, VH, &str_len);
            if (str_id == nullptr)
                return error(env, a_err_enif_alloc);

            if (enif_get_string(env, VH, str_id, str_len, ERL_NIF_LATIN1)
                > 0) {
                auto it = nmap.find(str_id);

                ERL_NIF_TERM res;
                if (it != nmap.end())
                    res = mres(env, s, group[it->second], opts.ct);
                else
                    res = mres(env, s, empty, opts.ct);

                if (enif_is_identical(res, a_err_enif_alloc_binary)) {
                    enif_free(str_id);
                    return error(env, a_err_enif_alloc_binary);
                } else {
                    vec.push_back(res);
                }
            } else {
                enif_free(str_id);
                return error(env, a_err_enif_get_string);
            }

            enif_free(str_id);
        }
    }

    ERL_NIF_TERM list = enif_make_list_from_array(env, &vec[0], vec.size());
    return enif_make_tuple2(env, a_match, list);
}

//
// Get number of capturing groups we want to request from RE2.
//
// It's more efficient to avoid requesting all capturing groups if we need none
// or just the first one.
//
static int number_of_capturing_groups(
    int nr_groups, matchoptions::value_spec vs)
{
    switch (vs) {
    case matchoptions::VS_NONE:
        return 0;
    case matchoptions::VS_FIRST:
        return 1;
    default:
        return nr_groups;
    }
}

static ERL_NIF_TERM re2_match_impl(
    ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary sdata;

    if (enif_inspect_iolist_as_binary(env, argv[0], &sdata)) {
        const re2::StringPiece s((const char*)sdata.data, sdata.size);
        re2::RE2* re               = nullptr;
        Re2UniquePtr re_unique_ptr = nullptr;
        union re2_handle_union handle;
        ErlNifBinary pdata;

        matchoptions opts(env);
        if (argc == 3 && !parse_match_options(env, argv[2], opts))
            return enif_make_badarg(env);

        if (enif_get_resource(env, argv[1], re2_resource_type, &handle.vp)
            && handle.p->re != nullptr) {
            // Save existing RE2 obj for use in this function
            re = handle.p->re;

            if (opts.caseless)  // caseless allowed either in compile or match
                return enif_make_badarg(env);
        } else if (enif_inspect_iolist_as_binary(env, argv[1], &pdata)) {
            const re2::StringPiece p((const char*)pdata.data, pdata.size);
            re2::RE2::Options re2opts;
            re2opts.set_log_errors(false);
            if (opts.caseless)
                re2opts.set_case_sensitive(false);
            re2::RE2* re2 = (re2::RE2*)enif_alloc(sizeof(re2::RE2));
            if (re2 == nullptr)
                return error(env, a_err_enif_alloc);
            // Save temporary RE2 obj for use in this function
            re = new (re2) re2::RE2(p, re2opts);  // placement new
            // Save RE2 obj ptr for cleanup via unique_ptr
            re_unique_ptr.reset(re);
        } else {
            return enif_make_badarg(env);
        }

        if (!re->ok())
            return enif_make_badarg(env);

        // nr_groups must be the number of capturing groups + 1 because
        // group[0] will be the text matched by the entire pattern, group[1]
        // will be the first capturing group et cetera (assuming n >= 2), if
        // there are any capturing groups in the regex and opts.vs causes us to
        // request them.
        const int nr_groups = re->NumberOfCapturingGroups() + 1;
        const int n         = number_of_capturing_groups(nr_groups, opts.vs);
        std::vector<re2::StringPiece> group;
        group.reserve(n);

        if (re->Match(
                s,
                opts.offset,
                s.size(),
                re2::RE2::UNANCHORED,
                group.data(),
                n)) {

            int start = 0;
            int arrsz = n;

            if (opts.vs == matchoptions::VS_NONE) {

                // return match atom only

                return a_match;
            } else if (opts.vs == matchoptions::VS_FIRST) {

                // return first match only

                ERL_NIF_TERM first = mres(env, s, group[0], opts.ct);
                if (enif_is_identical(first, a_err_enif_alloc_binary)) {
                    return error(env, a_err_enif_alloc_binary);
                } else {
                    return enif_make_tuple2(
                        env, a_match, enif_make_list1(env, first));
                }
            } else if (opts.vs == matchoptions::VS_ALL_BUT_FIRST) {
                // skip first match
                start = 1;
                arrsz--;
            }

            if (opts.vs == matchoptions::VS_VLIST) {

                // return matched subpatterns as specified in ValueList

                return re2_match_ret_vlist(env, *re, s, opts, group, n);
            } else {

                // return all or all_but_first matches

                ERL_NIF_TERM* arr
                    = (ERL_NIF_TERM*)enif_alloc(sizeof(ERL_NIF_TERM) * n);
                for (int i = start, arridx = 0; i < n; i++, arridx++) {
                    ERL_NIF_TERM res = mres(env, s, group[i], opts.ct);
                    if (enif_is_identical(res, a_err_enif_alloc_binary)) {
                        enif_free(arr);
                        return error(env, a_err_enif_alloc_binary);
                    } else {
                        arr[arridx] = res;
                    }
                }

                ERL_NIF_TERM list = enif_make_list_from_array(env, arr, arrsz);
                enif_free(arr);

                return enif_make_tuple2(env, a_match, list);
            }
        } else {

            return a_nomatch;
        }
    } else {

        return enif_make_badarg(env);
    }
}

// ===========
// re2:replace
// ===========

//
// Options = [ Option ]
// Option = global
//
static bool parse_replace_options(
    ErlNifEnv* env, const ERL_NIF_TERM list, replaceoptions& opts)
{
    if (enif_is_empty_list(env, list))
        return true;

    ERL_NIF_TERM L, H, T;

    for (L = list; enif_get_list_cell(env, L, &H, &T); L = T) {

        if (enif_is_identical(H, a_global))
            opts.global = true;
        else
            return false;
    }

    return true;
}

//
// build result for re2:replace
//
static ERL_NIF_TERM rres(ErlNifEnv* env, const std::string& s)
{
    ErlNifBinary bsubst;
    if (!enif_alloc_binary(s.size(), &bsubst))
        return error(env, a_err_enif_alloc_binary);
    memcpy(bsubst.data, s.data(), s.size());
    return enif_make_binary(env, &bsubst);
}

static ERL_NIF_TERM re2_replace_impl(
    ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary sdata, rdata;

    if (enif_inspect_iolist_as_binary(env, argv[0], &sdata)
        && enif_inspect_iolist_as_binary(env, argv[2], &rdata)) {
        std::string s((const char*)sdata.data, sdata.size);
        const re2::StringPiece r((const char*)rdata.data, rdata.size);
        re2::RE2* re               = nullptr;
        Re2UniquePtr re_unique_ptr = nullptr;
        union re2_handle_union handle;
        ErlNifBinary pdata;

        if (enif_get_resource(env, argv[1], re2_resource_type, &handle.vp)
            && handle.p->re != nullptr) {
            // Save existing RE2 obj for use in this function
            re = handle.p->re;
        } else if (enif_inspect_iolist_as_binary(env, argv[1], &pdata)) {
            const re2::StringPiece p((const char*)pdata.data, pdata.size);
            re2::RE2::Options re2opts;
            re2opts.set_log_errors(false);
            re2::RE2* re2 = (re2::RE2*)enif_alloc(sizeof(re2::RE2));
            if (re2 == nullptr)
                return error(env, a_err_enif_alloc);
            // Save temporary RE2 obj for use in this function
            re = new (re2) re2::RE2(p, re2opts);  // placement new
            // Save RE2 obj ptr for cleanup via unique_ptr
            re_unique_ptr.reset(re);
        } else {
            return enif_make_badarg(env);
        }

        if (!re->ok())
            return enif_make_badarg(env);

        replaceoptions opts;
        if (argc == 4 && !parse_replace_options(env, argv[3], opts))
            return enif_make_badarg(env);

        if (opts.global) {
            if (re2::RE2::GlobalReplace(&s, *re, r)) {
                return rres(env, s);
            } else {
                return a_error;
            }
        } else {
            if (re2::RE2::Replace(&s, *re, r)) {
                return rres(env, s);
            } else {
                return a_error;
            }
        }
    } else {
        return enif_make_badarg(env);
    }
}

extern "C" {
static ERL_NIF_TERM re2_compile(
    ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    return SCHEDULE_NIF(
        env, "compile", ds_flags, &re2_compile_impl, argc, argv);
}

static ERL_NIF_TERM re2_match(
    ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    return SCHEDULE_NIF(env, "match", ds_flags, &re2_match_impl, argc, argv);
}

static ERL_NIF_TERM re2_replace(
    ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    return SCHEDULE_NIF(
        env, "replace", ds_flags, &re2_replace_impl, argc, argv);
}

static ErlNifFunc nif_funcs[] = {
    NIF_FUNC_ENTRY("compile", 1, re2_compile),
    NIF_FUNC_ENTRY("compile", 2, re2_compile),
    NIF_FUNC_ENTRY("match", 2, re2_match),
    NIF_FUNC_ENTRY("match", 3, re2_match),
    NIF_FUNC_ENTRY("replace", 3, re2_replace),
    NIF_FUNC_ENTRY("replace", 4, re2_replace),
};

static void re2_resource_cleanup(ErlNifEnv*, void* arg)
{
    // Release any dynamically allocated memory stored in re2_handle
    re2_handle* handle = (re2_handle*)arg;
    cleanup_handle(handle);
}

static int on_load(ErlNifEnv* env, void**, ERL_NIF_TERM)
{
    ErlNifResourceFlags flags
        = (ErlNifResourceFlags)(ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER);

    ErlNifResourceType* rt = enif_open_resource_type(
        env, nullptr, "re2_resource", &re2_resource_cleanup, flags, nullptr);

    if (rt == nullptr)
        return -1;

    re2_resource_type = rt;

    init_atoms(env);

    if (have_online_dirty_schedulers()) {
        DBG("dirty schedulers: online\n");
        ds_flags = DS_MODE;
    } else {
        DBG("dirty schedulers: offline or unsupported\n");
    }

    return 0;
}

ERL_NIF_INIT(re2, nif_funcs, &on_load, nullptr, nullptr, nullptr)
}  // extern "C"
