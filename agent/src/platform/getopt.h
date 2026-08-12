#pragma once

// getopt_long shim. On POSIX we use the system <getopt.h>; on Windows/MSVC we
// provide a self-contained implementation (agent/src/platform/getopt.cpp) that
// supports the subset of GNU getopt used by the agent (required/optional
// arguments, long-option unique-prefix abbreviation, clustered short options).
#ifdef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

struct option {
    const char *name;
    int has_arg;  // no_argument (0), required_argument (1), optional_argument (2)
    int *flag;    // if non-null, *flag is set to val and getopt_long returns 0
    int val;
};

#define no_argument 0
#define required_argument 1
#define optional_argument 2

int getopt(int argc, char *const argv[], const char *optstring);
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif

#else  // POSIX

#include <getopt.h>

#endif
