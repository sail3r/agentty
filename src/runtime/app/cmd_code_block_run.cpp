// cmd_code_block_run.cpp — the EFFECT half of the Ctrl+G code-block flow.
//
// update/code_blocks.cpp is the reducer: it decides WHICH block runs and
// returns cmd::run_code_block(...). This file is what that Cmd does when the
// runtime runs it — fork/exec on the real tty with a tee (POSIX), or the
// captured subprocess runner (Windows). It lived inside the reducer file,
// which put ~500 lines of fork, termios, signals and clock reads under
// update/: not called from a fold (it always ran as a task), but readable
// as if it were, and invisible to the elm_purity lint's file boundary.
//
// Nothing here is reached except through the Cmd. See update/code_blocks.cpp
// for the user-facing design (interactive-first, capture to the composer
// only on request, Windows degradation).

#include <maya/core/target.hpp>
#include "agentty/runtime/app/cmd_code_block_run.hpp"

#include "agentty/runtime/app/update/internal.hpp"

#if !MAYA_OS_WINDOWS
    #include <cerrno>
    #include <csignal>
    #include <cstdio>
    #include <cstring>
    #include <ctime>
    #include <poll.h>
    #include <sys/ioctl.h>
    #include <termios.h>
    #include <unistd.h>
    #include <maya/runtime.hpp>
#else
    #include <chrono>
    #include <cstdio>
    #include <io.h>
    #include <stop_token>
    #include <maya/runtime.hpp>
#endif

#include <string>
#include <utility>

#include <maya/style/theme.hpp>
#include <maya/widget/agentty/run_transcript.hpp>

#include "agentty/runtime/panel/code_blocks.hpp"
#include "agentty/runtime/win_shell_encode.hpp"
#include "agentty/tool/util/subprocess.hpp"

namespace agentty::app::cmd {

namespace cbp = agentty::code_blocks;

namespace {
// the real terminal with a stdout/stderr tee — fully interactive, output
// captured. Windows: non-interactive captured runner (subprocess), same
// as the bash tool.
#if !MAYA_OS_WINDOWS

// Run `command` via /bin/sh -c on the REAL terminal (we're inside maya's
// suspend: cooked tty, TUI escapes torn down). stdin is inherited — the
// actual tty — so line editing / password reads behave; sudo talks to
// /dev/tty directly either way. stdout+stderr are dup2'd onto a pipe the
// parent tees: each read chunk is written straight to the tty (live
// output) and appended to the capture buffer. Returns the finished Msg.
// The run's chrome (frames, pinned status row, title, colour) is
// maya::RunTranscript's: it returns the bytes for each moment and this file
// only writes them. Colour honours NO_COLOR / TERM=dumb through the same
// detector the renderer uses, so the answer is consistent across the suspend
// boundary; off a colour tty the transcript is plain words.
namespace runner_ui {
    inline bool tty() {
        static const bool on = ::isatty(STDOUT_FILENO) == 1
            && maya::theme::detect_tier(/*tty=*/true)
                   != maya::theme::ColorTier::Mono;
        return on;
    }
    inline void emit(const std::string& s) {
        if (!s.empty()) (void)!::write(STDOUT_FILENO, s.data(), s.size());
    }
    inline int term_rows() {
        struct winsize ws{};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2)
            return ws.ws_row;
        return 0;   // unknown / too small: no pinned status row
    }
}

[[nodiscard]] CodeBlockRunFinished run_on_real_tty(const std::string& command) {
    namespace ui = runner_ui;
    CodeBlockRunFinished fin;
    fin.command = command;
    const maya::RunTranscript chrome{{.styled = ui::tty(), .rows = ui::term_rows()}};

    // Hand the child a CLEAN screen. maya's inline suspend leaves the last
    // TUI frame painted above the cursor; without this the header and child
    // output scroll up into it and overlap. clear() wipes the viewport
    // (scrollback preserved) and homes the cursor.
    ui::emit(chrome.clear());

    // Framed run header: a clearly-delimited banner so the user can see at
    // a glance WHERE the run started (the TUI just tore down, so a bare
    // "$ cmd" line was easy to lose) and, crucially, that Ctrl-C stops it.
    // The command still reads like a shell prompt so the transcript is
    // copy-paste-faithful.
    ui::emit(chrome.header(command));

    // Started by jaal: no inherited descriptors, default signals in the
    // child. It stays in OUR session and keeps our stdin, because it is a
    // foreground job on the user's terminal: Ctrl-C at the tty has to reach
    // it, and an interactive command has to be able to read keys.
    namespace pf = maya::platform;
    pf::process_spec spec;
    spec.argv         = {"/bin/sh", "-c", command};
    spec.stdin_from   = pf::stream_to::inherit;
    spec.stdout_to    = pf::stream_to::pipe;
    spec.merge_stderr = true;
    spec.new_session  = false;
    auto spawned = pf::posix_process::spawn(spec);
    if (!spawned) {
        fin.output    = std::string{"[failed to start: "} + std::strerror(spawned.error().native) + "]";
        fin.exit_code = -1;
        return fin;
    }
    auto& proc = *spawned;
    const int out_fd  = proc.stdout_handle()->get();
    const int exit_fd = proc.exit_handle().get();

    // ── Parent ── classic system() semantics: ignore INT/QUIT while the
    // child runs so Ctrl+C kills only the command, not agentty.
    struct sigaction ign{}, old_int{}, old_quit{};
    ign.sa_handler = SIG_IGN;
    ::sigaction(SIGINT,  &ign, &old_int);
    ::sigaction(SIGQUIT, &ign, &old_quit);

    // Tee loop: live to the tty, captured to the buffer. Bounded capture
    // (2 MB) so a runaway command can't OOM the composer — the SCREEN
    // still shows everything; only the buffer stops growing.
    //
    // We poll(2) with a 1s timeout instead of a bare blocking read so a
    // SILENT command (npm install fetching, a network wait, a `sleep`)
    // still shows a heartbeat — "⠿ running… 12s (Ctrl-C to stop)" redrawn
    // in place — instead of a frozen-looking screen. The heartbeat is
    // TTY-only and is wiped the instant real output or the exit status
    // arrives, so it never pollutes the captured buffer or piped output.
    constexpr std::size_t kCaptureMax = 2u * 1024 * 1024;
    bool truncated = false;
    char buf[8192];
    const auto started = ::time(nullptr);
    long last_status_secs = -1;
    int  spin = 0;
    // Short command label for the timers (first token, clipped).
    std::string label = command.substr(0, command.find_first_of(" \t\n"));
    if (label.size() > 24) label.resize(24);

    // Two always-on elapsed readouts, belt-and-suspenders so SOMETHING is
    // visible regardless of terminal quirks:
    //   1. OSC-2 window title — non-intrusive, but some multiplexers drop it.
    //   2. A DECSTBM-pinned bottom status row — universal; output scrolls
    //      above it. Enabled only when we can read the terminal height.
    ui::emit(chrome.begin_status());
    auto tick = [&] {
        const long secs = static_cast<long>(::time(nullptr) - started);
        if (secs == last_status_secs) return;
        last_status_secs = secs;
        ui::emit(chrome.tick(secs, label, spin++));
    };
    tick();
    // Exit conditions, in the order they matter:
    //   • EOF on the pipe — the normal case (child exited, no survivors).
    //   • CHILD DEAD + PIPE QUIET — the hang-proof case. EOF requires
    //     EVERY write end closed, and a grandchild the command left
    //     behind (`npm run dev &`, a daemonizing build, a watcher)
    //     inherits stdout/stderr and can hold the pipe open FOREVER
    //     after the child exits. Without this arm the TUI stayed
    //     suspended indefinitely — the reported "^G sometimes hangs".
    //     So: reap the child without blocking inside the loop; once it
    //     is dead and a full poll interval passes with no data, stop.
    //     A still-attached background survivor that writes later gets
    //     EPIPE/SIGPIPE — the price of restoring the UI; a command that
    //     WANTS a survivor should redirect its output (`… > log &`).
    bool child_dead = false;
    for (;;) {
        struct pollfd pfds[2] = {{out_fd, POLLIN, 0}, {exit_fd, POLLIN, 0}};
        const int pr = ::poll(pfds, child_dead ? 1 : 2, 1000);
        tick();   // refresh timers whether or not output arrived this tick
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!child_dead && (pfds[1].revents & (POLLIN | POLLHUP))) child_dead = true;
        if (!(pfds[0].revents & (POLLIN | POLLHUP))) {
            if (child_dead && pr <= 1) break;   // dead + quiet ≥ 1s → done
            continue;
        }
        const ssize_t n = ::read(out_fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (n == 0) break;
        (void)!::write(STDOUT_FILENO, buf, static_cast<std::size_t>(n));
        if (fin.output.size() < kCaptureMax) {
            const auto room = kCaptureMax - fin.output.size();
            fin.output.append(buf, std::min(static_cast<std::size_t>(n), room));
            if (static_cast<std::size_t>(n) > room) truncated = true;
        } else {
            truncated = true;
        }
    }
    ui::emit(chrome.end_status());

    if (!child_dead) {
        pollfd pe{exit_fd, POLLIN, 0};
        while (::poll(&pe, 1, -1) < 0 && errno == EINTR) {}
    }
    const auto status = proc.reap();
    ::sigaction(SIGINT,  &old_int,  nullptr);
    ::sigaction(SIGQUIT, &old_quit, nullptr);

    // Distinguish a user interrupt (Ctrl-C / Ctrl-\) from a genuine
    // command failure — "failed exit 130" reads like the command broke
    // when the user actually stopped it on purpose.
    bool interrupted = false;
    if (!status) {
        fin.exit_code = -1;
    } else if (status->how == pf::exit_status::kind::exited) {
        fin.exit_code = status->code;
    } else {
        interrupted = (status->code == SIGINT || status->code == SIGQUIT);
        fin.exit_code = 128 + status->code;
    }
    if (truncated) fin.output += "\n[capture truncated at 2 MB — full output was shown on screen]";

    // Completion footer: a colour-coded status line so success/failure/
    // interrupt is unmistakable at a glance, with elapsed time.
    {
        const long secs = static_cast<long>(::time(nullptr) - started);
        using O = maya::RunTranscript::Outcome;
        const O o = interrupted ? O::Stopped : fin.exit_code == 0 ? O::Done : O::Failed;
        ui::emit(chrome.footer(o, fin.exit_code, secs));
    }

    // Keypress-to-continue: hold the transcript on screen until the user
    // acknowledges, so a fast command's output isn't repainted away before
    // it can be read. Only when stdout AND stdin are a real terminal (so
    // we can actually read a key); piped/redirected runs skip it. The
    // prompt is on its own line and cleared afterward so the restored TUI
    // starts on a clean row. Any key continues.
    if (ui::tty() && ::isatty(STDIN_FILENO) == 1) {
        ui::emit(chrome.continue_prompt());
        // Best-effort raw single-key read: disable canonical mode + echo
        // so a lone keypress (no Enter) continues, then restore.
        struct termios oldt{}, raw{};
        const bool have_termios = (::tcgetattr(STDIN_FILENO, &oldt) == 0);
        if (have_termios) {
            raw = oldt;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN]  = 1;
            raw.c_cc[VTIME] = 0;
            ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        char c;
        // EINTR retries; EOF (0) and real errors (EIO after a hangup) fall
        // through to the restore path — this gate can stall only for a live
        // tty with a human who hasn't pressed a key yet, which is its job.
        while (::read(STDIN_FILENO, &c, 1) < 0 && errno == EINTR) {}
        if (have_termios) ::tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        ui::emit(chrome.clear_line());
    }
    return fin;
}

[[nodiscard]] Cmd run_block_cmd(std::string command, cbp::BlockShell /*shell*/) {
    return Cmd::suspend(
        [cmd = std::move(command)]() -> Msg {
            return Msg{run_on_real_tty(cmd)};
        });
}

#else  // MAYA_OS_WINDOWS — non-interactive fallback via the shared subprocess runner

#include <maya/runtime.hpp>

// Wrap the block body for the chosen Windows interpreter. cmd.exe is the
// default shell of run_command_s (it wraps in `cmd.exe /S /C "..."`), so
// a Cmd block passes through verbatim. A PowerShell block is handed to
// powershell -NoProfile -Command; we base64-encode it via -EncodedCommand
// so arbitrary quoting/newlines survive the cmd.exe wrapper intact
// (nested quotes through cmd /C are the classic breakage). Multi-line
// scripts Just Work because the whole body is one encoded argument.
[[nodiscard]] std::string wrap_for_windows_shell(cbp::BlockShell shell,
                                                 const std::string& body) {
    if (shell != cbp::BlockShell::PowerShell) return body;  // Cmd: verbatim
    // PowerShell -EncodedCommand contract (base64 of UTF-16LE) lives in a
    // shared, platform-independent header so it can be verified off Windows
    // — the header carries a compile-time known-answer static_assert block
    // that fails THIS build if the transform ever regresses.
    return win_shell::powershell_command(body);
}

// The task BODY for the Windows runner. jaal requires a task body to capture
// nothing (`TaskBody<Body, Msg, Args...>` resolves to a plain function
// pointer), so everything it needs arrives as Sendable arguments after the
// stop_token. This used to be a capturing lambda, which is why the msys2 leg
// — the only CI lane with a compiler new enough to instantiate jaal's checks
// — failed with "a task body must not capture anything" while MSVC reported
// the same thing as a Sendable static_assert.
//
// The console-echo helper below is also deliberately NOT named `out`: the
// Sink parameter owns that name, and shadowing it made `out.send(...)`
// resolve to the echo lambda.
static void run_block_body(const maya::unlocked& cap, maya::Sink<Msg> out, std::stop_token stop,
                           std::string cmd, cbp::BlockShell shell) {
    const std::string wrapped = wrap_for_windows_shell(shell, cmd);

    // Windows parity for "what's happening while it runs": the
    // shared subprocess runner (run_command_s) is blocking and
    // non-streaming — output only appears in the Result card at the
    // end — so we can't tee live. But we CAN show the run is alive:
    // print a header, spin a background thread that ticks an
    // elapsed-time heartbeat (OSC-2 title + an in-place line;
    // modern Windows Terminal / conhost with VT processing render
    // both), run the command, then a footer. All decoration is
    // console-only and never enters the captured buffer.
    const bool con = (::_isatty(_fileno(stdout)) != 0);
    const maya::RunTranscript chrome{{.styled = con}};
    auto echo = [](const std::string& s) {
        if (!s.empty()) { std::fwrite(s.data(), 1, s.size(), stdout); std::fflush(stdout); }
    };
    if (con) {
        echo(chrome.captured_header(cmd));
    }

    std::string label = cmd.substr(0, cmd.find_first_of(" \t\n"));
    if (label.size() > 24) label.resize(24);

    // The heartbeat runs in a maya::scope, not a bare std::thread.
    //
    // The old shape was `std::thread ticker; ... ticker.join()` with the
    // blocking run_command_s between them. Two problems, both invisible until
    // they bite: if anything in that window threw, ~thread ran on a joinable
    // thread and the process called std::terminate (no stack, no message);
    // and the helper captured `done_flag` and `label` by reference, which is
    // only safe because of that same join nobody was guaranteed to reach.
    //
    // scope() joins every helper on EVERY exit path — normal return, early
    // return, exception — which is what makes the [&] capture of locals
    // legitimate rather than lucky. There is also no detach() to reach for,
    // so the helper cannot be made to outlive the frame it borrows from.
    //
    // The task's own stop_token is passed in as the parent, so Esc (or
    // shutdown) cancels the heartbeat through the same path as everything
    // else instead of needing its own flag. done_flag stays for the normal
    // "the command finished" stop, which is not a cancellation.
    struct Outcome {
        tools::util::SubprocessResult r;
        long                          secs = 0;
    };

    // What the heartbeat shares with this thread: one stop flag. Sync
    // (const stop_source), held by a co_owned so neither side can free it
    // under the other. request_stop() is non-const in the standard (only
    // libstdc++ marks it const), so it goes through a copy: copies share the
    // stop state, and copying a const source is allowed from any thread.
    struct Beat { const std::stop_source done; };
    auto beat_state = maya::co_owned<Beat>::make();

    auto outcome = maya::scope(cap, stop, [&](maya::nursery& n) {
        // The helper's own token comes from the scope's, so Esc reaches the
        // spinner too; the callback folds it into `done`.
        auto beat = n.spawn([](const maya::unlocked& hcap, std::stop_token st, maya::co_owned<Beat> b, std::string label,
                               bool con) {
            if (!con) return;
            const std::stop_callback on_stop(st, [&b] { std::stop_source(b->done).request_stop(); });
            const maya::RunTranscript chrome{{.styled = true}};
            int spin = 0;
            const auto start = std::chrono::steady_clock::now();
            do {
                const long secs = static_cast<long>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start).count());
                const std::string s = chrome.inline_tick(secs, label, spin++);
                std::fwrite(s.data(), 1, s.size(), stdout); std::fflush(stdout);
            } while (!maya::delay_for(hcap, b->done.get_token(), std::chrono::milliseconds(250)));
        }, beat_state, label, con);

        // However the command ends (or throws), stop the spinner before the
        // handle joins it.
        struct StopBeat {
            const Beat& b;
            ~StopBeat() { std::stop_source(b.done).request_stop(); }
        } stop_beat{*beat_state};

        const auto start = std::chrono::steady_clock::now();
        Outcome o;
        o.r = tools::util::run_command_s(wrapped);
        o.secs = static_cast<long>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count());

        std::stop_source(beat_state->done).request_stop();
        beat.join();          // explicit, so a throwing helper surfaces here
        return o;
    });

    auto r          = std::move(outcome.r);
    const long secs = outcome.secs;

    CodeBlockRunFinished fin;
    fin.command   = cmd;   // show the ORIGINAL body in the card
    fin.timed_out = r.timed_out;
    if (!r.started) {
        fin.output    = "[failed to start: " + r.start_error + "]";
        fin.exit_code = -1;
    } else {
        fin.output    = std::move(r.output);
        fin.exit_code = r.exit_code;
        if (r.truncated) fin.output += "\n[output truncated]";
    }

    if (con) {
        using O = maya::RunTranscript::Outcome;
        const O o = r.timed_out ? O::TimedOut : fin.exit_code == 0 ? O::Done : O::Failed;
        echo(chrome.end_inline() + chrome.footer(o, fin.exit_code, secs, /*fresh_line=*/false));
    }

    out.send(Msg{std::move(fin)});
}

[[nodiscard]] Cmd run_block_cmd(std::string command, cbp::BlockShell shell) {
    // Body is a captureless lambda over the plain function, so jaal hands it
    // the worker's `unlocked` (the heartbeat scope joins: it needs one); the
    // command and shell ride along as Sendable arguments.
    return Cmd::task_isolated(
        [](const maya::unlocked& cap, maya::Sink<Msg> out, std::stop_token stop,
           std::string cmd, cbp::BlockShell sh) {
            run_block_body(cap, std::move(out), std::move(stop), std::move(cmd), sh);
        },
        std::move(command), shell);
}

#endif
} // namespace

Cmd run_code_block(std::string command, cbp::BlockShell shell) {
    return run_block_cmd(std::move(command), shell);
}

} // namespace agentty::app::cmd
