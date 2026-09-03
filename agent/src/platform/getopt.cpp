#ifdef _WIN32

#include "getopt.h"

#include <cstdio>
#include <cstring>

char *optarg = nullptr;
int optind = 1;
int opterr = 1;
int optopt = 0;

namespace {

const char *g_pos = nullptr;

int FindLong(const struct option *longopts, const char *name, size_t namelen) {
    int match = -1;
    bool ambiguous = false;
    for (int i = 0; longopts[i].name; ++i) {
        if (std::strncmp(longopts[i].name, name, namelen) != 0) continue;
        if (std::strlen(longopts[i].name) == namelen) {
            return i;
        }
        if (match != -1) {
            ambiguous = true;
        } else {
            match = i;
        }
    }
    return ambiguous ? -2 : match;
}

int FailUnknownLong(const char *name) {
    if (opterr) {
        std::fprintf(stderr, "pudim-agent: unrecognized option '--%s'\n", name);
    }
    optopt = '?';
    g_pos = nullptr;
    optind++;
    return '?';
}

} // namespace

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    optarg = nullptr;
    if (optind >= argc) return -1;

    const char *arg = argv[optind];
    if (g_pos == nullptr) {
        if (arg[0] != '-' || arg[1] == '\0') return -1;
        if (arg[1] == '-' && arg[2] == '\0') {
            optind++;
            return -1;
        }
        g_pos = arg + 1;
    }

    if (*g_pos == '-' && longopts != nullptr) {
        const char *name = g_pos + 1;
        const char *eq = std::strchr(name, '=');
        size_t namelen =
            eq ? static_cast<size_t>(eq - name) : std::strlen(name);
        int idx = FindLong(longopts, name, namelen);
        if (idx == -2) {
            if (opterr) {
                std::fprintf(stderr,
                             "pudim-agent: option '--%s' is ambiguous\n", name);
            }
            optopt = '?';
            g_pos = nullptr;
            optind++;
            return '?';
        }
        if (idx == -1) return FailUnknownLong(name);

        const struct option &lo = longopts[idx];
        if (longindex) *longindex = idx;

        if (lo.has_arg == required_argument) {
            if (eq) {
                optarg = const_cast<char *>(eq + 1);
            } else if (optind + 1 < argc) {
                optarg = argv[++optind];
            } else {
                if (opterr) {
                    std::fprintf(stderr,
                                 "pudim-agent: option '--%s' requires an "
                                 "argument\n",
                                 lo.name);
                }
                optopt = lo.val;
                g_pos = nullptr;
                optind++;
                return (optstring && optstring[0] == ':') ? ':' : '?';
            }
        } else if (lo.has_arg == optional_argument) {
            optarg = eq ? const_cast<char *>(eq + 1) : nullptr;
        } else if (eq) {
            if (opterr) {
                std::fprintf(stderr,
                             "pudim-agent: option '--%s' does not take an "
                             "argument\n",
                             lo.name);
            }
            optopt = lo.val;
            g_pos = nullptr;
            optind++;
            return '?';
        } else {
            optarg = nullptr;
        }

        g_pos = nullptr;
        optind++;
        if (lo.flag) {
            *lo.flag = lo.val;
            return 0;
        }
        return lo.val;
    }

    const char *p = optstring ? std::strchr(optstring, *g_pos) : nullptr;
    if (!p) {
        if (opterr) {
            std::fprintf(stderr, "pudim-agent: invalid option -- '%c'\n",
                         *g_pos);
        }
        optopt = static_cast<unsigned char>(*g_pos);
        g_pos = nullptr;
        optind++;
        return '?';
    }
    char c = *g_pos;
    bool takes_arg = (p[1] == ':');
    if (takes_arg) {
        if (g_pos[1] != '\0') {
            optarg = const_cast<char *>(g_pos + 1);
            g_pos = nullptr;
            optind++;
        } else if (optind + 1 < argc) {
            optarg = argv[optind + 1];
            g_pos = nullptr;
            optind += 2;
        } else {
            if (opterr) {
                std::fprintf(stderr,
                             "pudim-agent: option requires an argument -- '%c'\n",
                             c);
            }
            optopt = static_cast<unsigned char>(c);
            g_pos = nullptr;
            optind++;
            return (optstring[0] == ':') ? ':' : '?';
        }
    } else {
        optarg = nullptr;
        g_pos++;
        if (*g_pos == '\0') {
            g_pos = nullptr;
            optind++;
        }
    }
    return c;
}

int getopt(int argc, char *const argv[], const char *optstring) {
    return getopt_long(argc, argv, optstring, nullptr, nullptr);
}

#endif  // _WIN32
