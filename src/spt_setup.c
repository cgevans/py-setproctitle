/*-------------------------------------------------------------------------
 *
 * spt_setup.c
 *    Initalization code for the spt_status.c module functions.
 *
 * Copyright (c) 2009-2012 Daniele Varrazzo <daniele.varrazzo@gmail.com>
 *
 *-------------------------------------------------------------------------
 */

#include "spt_setup.h"

#include "spt.h"
#include "spt_status.h"

/* Darwin doesn't export environ */
#if defined(__darwin__)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

#ifndef WIN32

/* return a concatenated version of a strings vector
 *
 * Return newly allocated heap space: clean it up with free()
 */
static char *
join_argv(int argc, char **argv)
{
    /* Calculate the final string length */
    int i;
    size_t len = 0;
    for (i = 0; i < argc; i++) {
        len += strlen(argv[i]) + 1;
    }

    char *buf = (char *)malloc(len);

    /* Copy the strings in the buffer joining with spaces */
    char *dest = buf;
    char *src;
    for (i = 0; i < argc; i++) {
        src = argv[i];
        while (*src) {
            *dest++ = *src++;
        }
        *dest++ = ' ';
    }
    *--dest = '\x00';

    return buf;
}


#ifdef IS_PY3K

/* Return a copy of argv[0] encoded in the default encoding.
 *
 * Return a newly allocated buffer to be released with free()
 */
static char *
get_encoded_arg0(wchar_t *argv0)
{
    PyObject *ua = NULL, *ba = NULL;
    char *rv = NULL;

    if (!(ua = PyUnicode_FromWideChar(argv0, -1))) {
        spt_debug("failed to convert argv[0] to unicode");
        goto exit;
    }

    if (!(ba = PyUnicode_AsEncodedString(
            ua, PyUnicode_GetDefaultEncoding(), "strict"))) {
        spt_debug("failed to encode argv[0]");
        goto exit;
    }

    rv = strdup(PyBytes_AsString(ba));

exit:
    PyErr_Clear();
    Py_XDECREF(ua);
    Py_XDECREF(ba);

    return rv;
}

#endif  /* IS_PY3K */


/* Find the original arg buffer starting from the env position.
 *
 * Return a malloc'd argv vector, pointing to the original arguments.
 *
 * Required on Python 3 as Py_GetArgcArgv doesn't return pointers to the
 * original area. It is also called on Python 2 as some cmdline parameters
 * mess up argv (issue #8).
 */
static char **
find_argv_from_env(int argc, char *arg0)
{
    char **buf = NULL;
    char **rv = NULL;

    spt_debug("walking from environ to look for the arguments");

    if (!(buf = (char **)malloc((argc + 1) * sizeof(char *)))) {
        spt_debug("can't malloc %d args!", argc);
        goto error;
    }
    buf[argc] = NULL;

    /* Walk back from environ until you find argc-1 null-terminated strings.
     * Don't look for argv[0] as it's probably not preceded by 0. */
    int i;
    char *ptr = environ[0];
    spt_debug("found environ at %p", ptr);
    char *limit = ptr - 8192;  /* TODO: empiric limit: should use MAX_ARG */
    --ptr;
    for (i = argc - 1; i >= 1; --i) {
        if (*ptr) {
            spt_debug("zero %d not found", i);
            goto error;
        }
        --ptr;
        while (*ptr && ptr > limit) { --ptr; }
        if (ptr <= limit) {
            spt_debug("failed to found arg %d start", i);
            goto error;
        }
        buf[i] = (ptr + 1);
        spt_debug("found argv[%d] at %p: %s", i, buf[i], buf[i]);
    }

    /* The first arg has not a zero in front. But what we have is reliable
     * enough (modulo its encoding). Check if it is exactly what found.
     *
     * The check is known to fail on OS X with locale C if there are
     * non-ascii characters in the executable path. See Python issue #9167
     */
    ptr -= strlen(arg0);
    spt_debug("argv[0] should be at %p", ptr);

    if (ptr <= limit) {
        spt_debug("failed to found argv[0] start");
        goto error;
    }
    if (strcmp(ptr, arg0)) {
        spt_debug("argv[0] doesn't match '%s'", arg0);
        goto error;
    }

    /* We have all the pieces of the jigsaw. */
    buf[0] = ptr;
    spt_debug("found argv[0]: %s", buf[0]);
    rv = buf;
    buf = NULL;

error:
    if (buf) { free(buf); }

    return rv;
}


#ifdef IS_PY3K

/* Come on, why is this missing?! this is just cruel!
 * I guess you club seal pups for hobby. */
PyObject *
PyFile_FromString(const char *filename, const char *mode)
{
    PyObject *io = NULL;
    PyObject *rv = NULL;

    if (!(io = PyImport_ImportModule("io"))) {
        spt_debug("failed to import io");
        goto exit;
    }

    rv = PyObject_CallMethod(io, "open", "ss", filename, mode);

exit:
    Py_XDECREF(io);
    return rv;
}

#endif  /* IS_PY3K */

/* Read the number of arguments and the first argument from /proc/pid/cmdline
 *
 * Return 1 if found, else 0. Return arg0 in a malloc'd array.
 */
static int
get_args_from_proc(int *argc_o, char **arg0_o)
{
    /* allow /proc/XXXXX/cmdline */
#define FNLEN 20
    char fn[FNLEN];

    PyObject *os = NULL;
    PyObject *pid_py = NULL;
    long pid;
    PyObject *f = NULL;
    PyObject *cl = NULL;

    PyObject *tmp = NULL;
    int rv = 0;

    spt_debug("looking for args into proc fs");

    /* get the pid from os.getpid() */
    if (!(os = PyImport_ImportModule("os"))) {
        spt_debug("failed to import os");
        goto exit;
    }
    if (!(pid_py = PyObject_CallMethod(os, "getpid", NULL))) {
        spt_debug("calling os.getpid() failed");
        goto exit;
    }
    if (-1 == (pid = PyInt_AsLong(pid_py))) {
        spt_debug("os.getpid() returned crap?");
        /* Don't bother to check PyErr_Occurred as pid can't just be -1. */
        goto exit;
    }

    /* get the content of /proc/PID/cmdline */
    snprintf(fn, FNLEN, "/proc/%ld/cmdline", pid);
    if (!(f = PyFile_FromString(fn, "rb"))) {
        spt_debug("opening '%s' failed", fn);
        goto exit;
    }
    /* the file has been open in binary mode, so we get bytes */
    cl = PyObject_CallMethod(f, "read", NULL);
    if (!(tmp = PyObject_CallMethod(f, "close", NULL))) {
        spt_debug("closing failed");
    }
    else {
        Py_DECREF(tmp);
    }

    if (!cl) {
        spt_debug("reading failed");
        goto exit;
    }

    /* the cmdline is a buffer of null-terminated strings. We can strdup it to
     * get a copy of arg0, and count the zeros to get argc */
    {
        char *ccl;
        Py_ssize_t i;

        if (!(ccl = Bytes_AsString(cl))) {
            spt_debug("failed to get cmdline string");
            goto exit;
        }
        if (!(*arg0_o = strdup(ccl))) {
            spt_debug("arg0 strdup failed");
            goto exit;
        }
        spt_debug("got argv[0] = '%s' from /proc", *arg0_o);

        *argc_o = 0;
        for (i = Bytes_Size(cl) - 1; i >= 0; --i) {
            if (ccl[i] == '\0') { (*argc_o)++; }
        }
        spt_debug("got argc = %d from /proc", *argc_o);
    }

    /* success */
    rv = 1;

exit:
    /* clear the exception. Propagating it to the module init would make a
     * fatal error. What we want instead is just to create a no-op module. */
    PyErr_Clear();

    Py_XDECREF(cl);
    Py_XDECREF(f);
    Py_XDECREF(pid_py);
    Py_XDECREF(os);

    return rv;
}

/* Find the original arg buffer, return nonzero if found.
 *
 * If found, set argc to the number of arguments, argv to an array
 * of pointers to the single arguments. The array is allocated via malloc.
 *
 * The function overcomes two Py_GetArgcArgv shortcomings:
 * - some python parameters mess up with the original argv, e.g. -m
 *   (see issue #8)
 * - with Python 3, argv is a decoded copy and doesn't point to
 *   the original area.
 * - If python is embedded, the function doesn't return anything.
 */
static int
get_argc_argv(int *argc_o, char ***argv_o)
{
    int argc = 0;
    argv_t **argv_py = NULL;
    char **argv = NULL;
    char *arg0 = NULL;
    int rv = 0;

    spt_debug("reading argc/argv from Python main");
    Py_GetArgcArgv(&argc, &argv_py);

    if (argc > 0) {
        spt_debug("found %d arguments", argc);

#ifdef IS_PY3K
        arg0 = get_encoded_arg0(argv_py[0]);
#else
        arg0 = strdup(argv_py[0]);
#endif
        if (!arg0) {
            spt_debug("couldn't get a copy of argv[0]");
            goto exit;
        }
    }
    else {
        spt_debug("no good news from Py_GetArgcArgv");

        if (!get_args_from_proc(&argc, &arg0)) {
            spt_debug("failed to get args from proc fs");
            goto exit;
        }
    }

    if (!(argv = find_argv_from_env(argc, arg0))) {
        spt_debug("couldn't find argv from environ");
        goto exit;
    }

    /* success */
    *argc_o = argc;
    *argv_o = argv;
    argv = NULL;
    rv = 1;

exit:
    if (arg0) { free(arg0); }
    if (argv) { free(argv); }

    return rv;
}

#endif  /* !WIN32 */


/* Initialize the module internal functions.
 *
 * The function reproduces the initialization performed by PostgreSQL
 * to be able to call the functions in pg_status.c
 *
 * The function should be called only once in the process lifetime.
 * so is called at module initialization. After the function is called,
 * set_ps_display() can be used.
 */
void
spt_setup(void)
{
#ifndef WIN32
    int argc = 0;
    char **argv = NULL;

    if (!get_argc_argv(&argc, &argv)) {
        spt_debug("setup failed");
        return;
    }

    save_ps_display_args(argc, argv);

    /* Set up the first title to fully initialize the code */
    char *init_title = join_argv(argc, argv);
    init_ps_display(init_title);
    free(init_title);
#else
    /* On Windows save_ps_display_args is a no-op
     * This is a good news, because Py_GetArgcArgv seems not usable.
     */
    LPTSTR init_title = GetCommandLine();
    init_ps_display(init_title);
#endif

}

