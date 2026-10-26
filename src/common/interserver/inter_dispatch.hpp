#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include "byte_stream.hpp"

// Flat opcode -> handler table (mirrors the single Master MsgProc switch, spec 03).
// Source peer is identified by the connection's peer index, NOT by the frame.
namespace paula::inter {

class InterLink;

// handler(link, sourcePeerIndex, bodyReader)
using Handler = std::function<void(InterLink&, int, StreamReader&)>;

class Dispatcher {
public:
    void on(uint16_t opcode, Handler h) { handlers_[opcode] = std::move(h); }

    // returns false if no handler is registered for this opcode
    bool dispatch(InterLink& link, int peerIndex, uint16_t opcode, StreamReader& r) {
        auto it = handlers_.find(opcode);
        if (it == handlers_.end()) return false;
        it->second(link, peerIndex, r);
        return true;
    }
private:
    std::unordered_map<uint16_t, Handler> handlers_;
};

} // namespace paula::inter
