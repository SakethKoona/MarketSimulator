#pragma once
// Command language shared by shell, run and connect modes.
#include "session.hpp"
#include <iosfwd>
#include <string>
#include <vector>

class Shell {
  public:
    Shell(Session &session, std::ostream &out, std::ostream &err);

    // Executes one line. Returns false when the session should end (quit).
    bool Execute(const std::string &line);

    // Reads lines until EOF or quit. Interactive mode prints a prompt.
    // Returns the number of lines that failed (parse or rejected).
    int Repl(std::istream &in, bool interactive);

    bool echo_events = false;
    int failures = 0; // parse errors and failed expectations

  private:
    using Args = std::vector<std::string>;
    bool cmd_order(const Args &a, Side side);
    bool cmd_cancel(const Args &a);
    bool cmd_modify(const Args &a);
    bool cmd_book(const Args &a, bool l2);
    bool cmd_top();
    bool cmd_order_info(const Args &a);
    bool cmd_events(const Args &a);
    bool cmd_flow(const Args &a);
    bool cmd_symbols();
    void help();

    void report(const OrderOutcome &o);
    void note_result(bool ok) { last_ok_ = ok; have_result_ = true; }
    bool last_ok_ = true;
    bool have_result_ = false;
    void print_fills();
    void print_events(std::size_t last_n);
    bool need_inspect();
    bool fail(const std::string &msg);

    Session &s_;
    std::ostream &out_;
    std::ostream &err_;
};
