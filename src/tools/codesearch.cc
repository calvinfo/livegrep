/********************************************************************
 * livegrep -- main.cc
 * Copyright (c) 2011-2013 Nelson Elhage
 *
 * This program is free software. You may use, redistribute, and/or
 * modify it under the terms listed in the COPYING file.
 ********************************************************************/
#include "codesearch.h"
#include "git_indexer.h"
#include "timer.h"
#include "re_width.h"

#include "interface.h"

#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <semaphore.h>

#include <iostream>

#include <gflags/gflags.h>

#include <re2/regexp.h>
#include "re2/walker-inl.h"

DEFINE_bool(json, false, "Use JSON output.");
DEFINE_int32(concurrency, 16, "Number of concurrent queries to allow.");
DEFINE_string(dump_index, "", "Dump the produced index to a specified file");
DEFINE_string(load_index, "", "Load the index from a file instead of walking the repository");
DEFINE_bool(quiet, false, "Do the search, but don't print results.");
DEFINE_string(listen, "", "Listen on a UNIX socket for connections");
DEFINE_string(name, "", "The name of this livegrep index");

using namespace std;
using namespace re2;

void die_errno(const char *str) {
    perror(str);
    exit(1);
}

struct print_match {
    print_match(codesearch_interface *ui) : ui_(ui) {}

    void operator()(const match_result *m) const {
        if (FLAGS_quiet)
            return;
        ui_->print_match(m);
    }
protected:
    codesearch_interface *ui_;
};

const int kMaxProgramSize = 4000;
const int kMaxWidth       = 200;

sem_t interact_sem;

void interact(code_searcher *cs, codesearch_interface *ui) {
    code_searcher::search_thread search(cs);
    WidthWalker width;

    RE2::Options opts;
    default_re2_options(opts);

    while (true) {
        ui->print_prompt(cs);
        string input;
        if (!ui->getline(input))
            break;

        string line, file, tree;
        if (!ui->parse_query(input, line, file, tree))
            continue;

        RE2 re(line, opts);
        RE2 file_re(file, opts);
        RE2 tree_re(tree, opts);
        if (!re.ok()) {
            ui->print_error(re.error());
            continue;
        }
        if (!file_re.ok()) {
            ui->print_error(file_re.error());
            continue;
        }
        if (!tree_re.ok()) {
            ui->print_error(tree_re.error());
            continue;
        }
        if (re.ProgramSize() > kMaxProgramSize) {
            ui->print_error("Parse error.");
            continue;
        }
        int w = width.Walk(re.Regexp(), 0);
        if (w > kMaxWidth) {
            ui->print_error("Parse error.");
            continue;
        }
        {
            timer tm;
            struct timeval elapsed;
            match_stats stats;

            ui->info("ProgramSize: %d\n", re.ProgramSize());

            {
                sem_wait(&interact_sem);
                search.match(re,
                             file.size() ? &file_re : 0,
                             tree.size() ? &tree_re : 0,
                             print_match(ui), &stats);
                sem_post(&interact_sem);
            }
            elapsed = tm.elapsed();
            ui->print_stats(elapsed, &stats);
        }
    }
}

struct parse_spec {
    string path;
    string name;
    vector<string> revs;
};

parse_spec parse_walk_spec(string spec) {
    /* [name@]path[:rev1,rev2,rev3] */
    parse_spec out;
    int idx;
    if ((idx = spec.find('@')) != -1) {
        out.name = spec.substr(0, idx);
        spec = spec.substr(idx + 1);
    }
    if ((idx = spec.find(':')) != -1) {
        string revs = spec.substr(idx + 1);
        spec = spec.substr(0, idx);
        while ((idx = revs.find(',')) != -1) {
            out.revs.push_back(revs.substr(0, idx));
            revs = revs.substr(idx + 1);
        }
        if (revs.size())
            out.revs.push_back(revs);
    }
    if (out.revs.empty()) {
        out.revs.push_back("HEAD");
    }
    out.path = spec;
    return out;
}

void walk_one(code_searcher *search,
              codesearch_interface *ui,
              string spec) {
    parse_spec parsed = parse_walk_spec(spec);
    ui->info("Walking `%s' (name: %s, path: %s)...\n",
             spec.c_str(),
             parsed.name.c_str(),
             parsed.path.c_str());
    git_indexer indexer(search, parsed.path, parsed.name);
    for (auto it = parsed.revs.begin(); it != parsed.revs.end(); ++it) {
        ui->info("  %s...", it->c_str());
        indexer.walk(*it);
        ui->info("done\n");
    }
}

void initialize_search(code_searcher *search,
                       codesearch_interface *ui,
                       int argc, char **argv) {
    if (FLAGS_load_index.size() == 0) {
        if (FLAGS_dump_index.size())
            search->set_alloc(make_dump_allocator(search, FLAGS_dump_index));
        else
            search->set_alloc(make_mem_allocator());

        timer tm;
        struct timeval elapsed;
        for (int i = 1; i < argc; i++) {
            walk_one(search, ui, argv[i]);
        }
        ui->info("Finalizing...\n");
        search->finalize();
        elapsed = tm.elapsed();
        ui->info("repository indexed in %d.%06ds\n",
                 (int)elapsed.tv_sec, (int)elapsed.tv_usec);
    } else {
        search->load_index(FLAGS_load_index);
    }
    if (!FLAGS_json && !FLAGS_load_index.size())
        search->dump_stats();
    if (FLAGS_dump_index.size() && FLAGS_load_index.size())
        search->dump_index(FLAGS_dump_index);
}

struct child_state {
    int fd;
    code_searcher *search;
};

void *handle_client(void *data) {
    child_state *child = static_cast<child_state*>(data);
    FILE *r = fdopen(child->fd, "r");
    FILE *w = fdopen(dup(child->fd), "w");
    codesearch_interface *interface;
    if (FLAGS_json)
        interface = make_json_interface(r, w);
    else
        interface = make_cli_interface(r, w);
    interact(child->search, interface);
    delete interface;
    delete child;
    fclose(r);
    fclose(w);
    return 0;
}

int bind_to_address(string spec) {
    int off = spec.find("://");
    string proto, address;

    if (off == string::npos) {
        proto = "unix";
        address = spec;
    } else {
        proto = spec.substr(0, off);
        address = spec.substr(off + 3);
    }

    int server;

    if (proto == "unix") {
        server = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server < 0)
            die_errno("socket(AF_UNIX)");

        struct sockaddr_un addr;

        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, address.c_str(), min(address.size(), sizeof(addr.sun_path) - 1));

        if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
            die_errno("Unable to bind socket");
    } else if (proto == "tcp") {
        int colon = address.find(':');
        if (colon == string::npos) {
            fprintf(stderr, "-listen: TCP addresses must be HOST:PORT.\n");
            exit(1);
        }
        string host = address.substr(0, colon);
        struct addrinfo hint = {};
        hint.ai_family = AF_INET;
        hint.ai_socktype = SOCK_STREAM;

        struct addrinfo *addrs = NULL;
        int err;
        if ((err = getaddrinfo(host.c_str(), address.c_str() + colon + 1,
                               &hint, &addrs)) != 0) {
            fprintf(stderr, "Error resolving %s: %s\n", host.c_str(), gai_strerror(err));
        }

        server = socket(addrs->ai_family, addrs->ai_socktype, addrs->ai_protocol);
        if (server < 0)
            die_errno("Creating socket");
        if (::bind(server, addrs->ai_addr, addrs->ai_addrlen) != 0)
            die_errno("Binding to address");
        freeaddrinfo(addrs);
    } else {
        fprintf(stderr, "Unknown protocol: %s\n", proto.c_str());
        exit(1);
    }

    if (listen(server, 4) < 0)
        die_errno("listen()");

    return server;
}

void listen(code_searcher *search, string path) {
    int server = bind_to_address(path);

    while(1) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0)
            die_errno("accept");

        child_state *state = new child_state;
        state->fd = fd;
        state->search = search;

        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, state);
    }
}

int main(int argc, char **argv) {
    google::SetUsageMessage("Usage: " + string(argv[0]) + " <options> REFS");
    google::ParseCommandLineFlags(&argc, &argv, true);

    prctl(PR_SET_PDEATHSIG, SIGINT);

    code_searcher search;
    codesearch_interface *interface = 0;
    if (FLAGS_json)
        interface = make_json_interface(stdin, stdout);
    else
        interface = make_cli_interface(stdin, stdout);

    signal(SIGPIPE, SIG_IGN);

    initialize_search(&search, interface, argc, argv);

    if (sem_init(&interact_sem, 0, FLAGS_concurrency) < 0)
        die_errno("sem_init");

    if (FLAGS_listen.size())
        listen(&search, FLAGS_listen);
    else
        interact(&search, interface);

    return 0;
}
