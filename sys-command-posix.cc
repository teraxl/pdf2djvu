/* Copyright © 2007-2015 Jakub Wilk <jwilk@jwilk.net>
 * Copyright © 2009 Mateusz Turcza
 *
 * This file is part of pdfdjvu.
 *
 * pdf2djvu is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * pdf2djvu is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#if !WIN32

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#if _OPENMP
#include <omp.h>
#endif

#include "array.hh"
#include "i18n.hh"
#include "system.hh"

Command::Command(const std::string& command) : command(command)
{
    this->argv.push_back(command);
}

Command &Command::operator <<(const std::string& arg)
{
    this->argv.push_back(arg);
    return *this;
}

Command &Command::operator <<(const File& arg)
{
    this->argv.push_back(arg);
    return *this;
}

Command &Command::operator <<(int i)
{
    std::ostringstream stream;
    stream << i;
    return *this << stream.str();
}

static int get_max_fd()
{
    int max_fd_per_thread = 32; // rough estimate
#if _OPENMP
    return max_fd_per_thread * omp_get_num_threads();
#else
    return max_fd_per_thread;
#endif
}

static void fd_close(int fd)
{
    int rc = close(fd);
    if (rc < 0)
        throw_posix_error("close()");
}

static void mkfifo(int fd[2], int flags=0)
{
    int rc = pipe(fd);
    if (rc < 0)
        throw_posix_error("pipe()");
    if (!flags)
        return;
    for (int i = 0; i < 2; i++) {
        int fdflags = fcntl(fd[i], F_GETFD);
        if (fdflags < 0)
            throw_posix_error("fcntl()");
        fdflags |= flags;
        int rc = fcntl(fd[i], F_SETFD, fdflags);
        if (rc < 0)
            throw_posix_error("fcntl()");
    }
}

static int fd_set_cloexec(int fd_from, int fd_to)
{
    for (int fd = fd_from; fd <= fd_to; fd++) {
        int fdflags = fcntl(fd, F_GETFD);
        if (fdflags < 0) {
            if (errno == EBADF)
                continue;
            return -1;
        }
        fdflags |= FD_CLOEXEC;
        int rc = fcntl(fd, F_SETFD, fdflags);
        if (rc < 0)
            return rc;
    }
    return 0;
}

static void report_posix_error(int fd, const char *context)
{
    int errno_copy = errno;
    ssize_t n = write(fd, &errno_copy, sizeof errno_copy);
    if (n != sizeof errno_copy)
        return;
    write(fd, context, strlen(context));
}

const std::string signame(int sig)
{
    switch (sig) {
#define s(n) case n: return "" # n "";
    /* POSIX.1-1990: */
    s(SIGHUP);
    s(SIGINT);
    s(SIGQUIT);
    s(SIGILL);
    s(SIGABRT);
    s(SIGFPE);
    s(SIGKILL);
    s(SIGSEGV);
    s(SIGPIPE);
    s(SIGALRM);
    s(SIGTERM);
    s(SIGUSR1);
    s(SIGUSR2);
    s(SIGCHLD);
    s(SIGCONT);
    s(SIGSTOP);
    s(SIGTSTP);
    s(SIGTTIN);
    s(SIGTTOU);
    /* SUSv2 and POSIX.1-2001: */
    s(SIGBUS);
    s(SIGPOLL);
    s(SIGPROF);
    s(SIGSYS);
    s(SIGTRAP);
    s(SIGURG);
    s(SIGVTALRM);
    s(SIGXCPU);
    s(SIGXFSZ);
#undef s
    default:
        return string_printf(_("signal %d"), sig);
    }
}

std::string Command::repr()
{
    bool sh = (
        this->argv.size() == 3 &&
        this->argv[0] == std::string("sh") &&
        this->argv[1] == std::string("-c")
    );
    if (sh)
        return this->argv[2];
    else
        return string_printf(
            // L10N: "<command> ..."
            _("%s ..."),
            this->command.c_str()
        );
}

void Command::call(std::istream *stdin_, std::ostream *stdout_, bool stderr_)
{
    int rc;
    int max_fd = get_max_fd();
    int stdout_pipe[2];
    int stdin_pipe[2];
    int error_pipe[2];
    size_t argc = this->argv.size();
    Array<const char *> c_argv(argc + 1);
    for (size_t i = 0; i < argc; i++)
        c_argv[i] = argv[i].c_str();
    c_argv[argc] = NULL;
    mkfifo(stdout_pipe);
    mkfifo(stdin_pipe, O_NONBLOCK);
    mkfifo(error_pipe);
    pid_t pid = fork();
    if (pid < 0)
        throw_posix_error("fork()");
    if (pid == 0) {
        /* The child:
         * At this point, only async-singal-safe functions can be used.
         * See the signal(7) manpage for the full list.
         */
        rc = dup2(stdin_pipe[0], STDIN_FILENO);
        if (rc < 0) {
            report_posix_error(error_pipe[1], "dup2()");
            abort();
        }
        rc = dup2(stdout_pipe[1], STDOUT_FILENO);
        if (rc < 0) {
            report_posix_error(error_pipe[1], "dup2()");
            abort();
        }
        if (!stderr_) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd < 0) {
                report_posix_error(error_pipe[1], "open()");
                abort();
            }
            rc = dup2(fd, STDERR_FILENO);
            if (rc < 0) {
                report_posix_error(error_pipe[1], "dup2()");
                abort();
            }
        }
        int rc = fd_set_cloexec(STDERR_FILENO + 1, max_fd);
        if (rc < 0) {
            report_posix_error(error_pipe[1], "fcntl()");
            abort();
        }
        execvp(c_argv[0],
            const_cast<char * const *>(
                static_cast<const char **>(c_argv)
            )
        );
        report_posix_error(error_pipe[1], "\xff");
        abort();
    }
    /* The parent: */
    fd_close(stdin_pipe[0]);
    fd_close(stdout_pipe[1]);
    fd_close(error_pipe[1]);
    char buffer[BUFSIZ];
    struct pollfd fds[2];
    if (stdin_)
        fds[0].fd = stdin_pipe[1];
    else {
        fds[0].fd = -1;
        fd_close(stdin_pipe[1]);
    }
    fds[0].events = POLLOUT;
    fds[1].fd = stdout_pipe[0];
    fds[1].events = POLLIN;
    while (1) {
        rc = poll(fds, 2, -1);
        if (rc < 0)
            throw_posix_error("poll()");
        if (fds[0].revents) {
            assert(stdin_);
            std::streamsize rbytes = stdin_->readsome(buffer, sizeof buffer);
            if (rbytes == 0) {
                fd_close(stdin_pipe[1]);
                fds[0].fd = -1;
            } else {
                ssize_t wbytes = write(stdin_pipe[1], buffer, rbytes);
                if (wbytes < 0)
                    throw_posix_error("write()");
                stdin_->seekg(wbytes - rbytes, std::ios_base::cur);
            }
        }
        if (fds[1].revents) {
            ssize_t nbytes = read(stdout_pipe[0], buffer, sizeof buffer);
            if (nbytes < 0)
                throw_posix_error("read()");
            if (nbytes == 0)
                break;
            if (stdout_)
                stdout_->write(buffer, nbytes);
        }
    }
    if (stdin_) {
        std::streamsize rbytes = stdin_->readsome(buffer, 1);
        if (rbytes > 0) {
            // The child process terminated,
            // even though it didn't receive the complete input.
            errno = EPIPE;
            throw_posix_error("write()");
        }
    }
    fd_close(stdout_pipe[0]);
    int wait_status;
    pid = waitpid(pid, &wait_status, 0);
    if (pid < 0)
        throw_posix_error("waitpid()");
    int child_errno = 0;
    ssize_t nbytes = read(error_pipe[0], &child_errno, sizeof child_errno);
    if (nbytes > 0 && static_cast<size_t>(nbytes) < sizeof child_errno) {
        errno = EIO;
        throw_posix_error("read()");
    }
    if (child_errno > 0) {
        char child_error_reason[BUFSIZ];
        ssize_t nbytes = read(
            error_pipe[0],
            child_error_reason,
            (sizeof child_error_reason) - 1
        );
        if (nbytes < 0)
            throw_posix_error("read()");
        fd_close(error_pipe[0]);
        child_error_reason[nbytes] = '\0';
        errno = child_errno;
        if (child_error_reason[0] != '\xff')
            throw_posix_error(child_error_reason);
        std::string child_error = POSIXError::error_message("");
        std::string message = string_printf(
            _("External command \"%s\" failed: %s"),
            this->repr().c_str(),
            child_error.c_str()
        );
        throw CommandFailed(message);
    }
    fd_close(error_pipe[0]);
    if (WIFEXITED(wait_status)) {
        unsigned long exit_status = WEXITSTATUS(wait_status);
        if (exit_status != 0) {
            std::string message = string_printf(
                _("External command \"%s\" failed with exit status %lu"),
                this->repr().c_str(),
                exit_status
            );
            throw CommandFailed(message);
        }
    } else if (WIFSIGNALED(wait_status)) {
        int sig = WTERMSIG(wait_status);
        std::string message = string_printf(
            // L10N: the latter argument is either an untranslated signal name
            // (such as "SIGSEGV") or a translated string "signal <N>"
            _("External command \"%s\" was terminated by %s"),
            this->repr().c_str(),
            signame(sig).c_str()
        );
        throw CommandFailed(message);
    } else {
        // should not happen
        errno = EINVAL;
        throw_posix_error("waitpid()");
    }
}

std::string Command::filter(const std::string &command_line, const std::string string)
{
    std::istringstream stdin_(string);
    std::ostringstream stdout_;
    Command cmd("sh");
    cmd << "-c" << command_line;
    cmd.call(&stdin_, &stdout_, true);
    return stdout_.str();
}

#endif

// vim:ts=4 sts=4 sw=4 et