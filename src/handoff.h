#pragma once
#include <windows.h>
#include <optional>
#include <string>
#include <vector>

// Handing a command line from a copy of the app that has just started to one already
// running, so that it is opened exactly once.
//
// The request is a ticket: the files, and a flag that the receiver claims and the
// sender may abandon, whichever comes first, in memory both copies can reach. The
// message (message()) carries only the ticket's id. It is sent first, which answers
// at once when the running copy is free; when that copy is busy the send times out,
// and Windows drops a sent message whose sender has stopped waiting for it, so it is
// also posted, which waits in the queue until that copy gets to it. Either may arrive,
// or both; only the first to claim the ticket opens the files. Meanwhile the sender
// may stop waiting (the user would rather open the files in a new window) by
// abandoning the ticket, after which the receiver refuses it.
namespace handoff {

// The registered window message; wParam and lParam identify the ticket
// (Ticket::wparam, lparam). 0 if it can't be registered, in which case no hand-off is
// possible.
UINT message();

struct TicketMemory;   // the shared part (handoff.cpp)

// The sender's side.
class Ticket {
public:
    explicit Ticket(const std::vector<std::wstring>& files);
    ~Ticket();
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;
    bool valid() const { return shared != nullptr; }
    // What the message carries: this process's id, and a serial no other ticket has had.
    WPARAM wparam() const { return (WPARAM)pid; }
    LPARAM lparam() const { return serial; }
    // Whether the receiver has taken the request.
    bool claimed() const;
    // Stop waiting. True if the request is now the sender's to open (the receiver will
    // refuse it), false if the receiver had already claimed it.
    bool abandon();
private:
    HANDLE mapping = nullptr;
    TicketMemory* shared = nullptr;
    DWORD pid = 0;
    LPARAM serial = 0;
};

// The receiver's side, given the message's wParam and lParam. The request's files if
// it is now this copy's to open; nothing if its sender gave up on it or has gone, it
// was claimed already (a second message for it), or the message names no ticket.
std::optional<std::vector<std::wstring>> claim(WPARAM wp, LPARAM lp);

} // namespace handoff
