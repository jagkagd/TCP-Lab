#include "tcp_connection.hh"

#include <iostream>

// Dummy implementation of a TCP connection

// For Lab 4, please replace with a real implementation that passes the
// automated checks run by `make check`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

size_t TCPConnection::remaining_outbound_capacity() const { return _sender.stream_in().remaining_capacity(); }

size_t TCPConnection::bytes_in_flight() const { return _sender.bytes_in_flight(); }

size_t TCPConnection::unassembled_bytes() const { return _receiver.unassembled_bytes(); }

size_t TCPConnection::time_since_last_segment_received() const { return _time_since_last_segment_received; }

void TCPConnection::segment_received(const TCPSegment &seg) {
    _time_since_last_segment_received = 0;
    if (seg.header().rst) {
        _sender.stream_in().set_error();
        _receiver.stream_out().set_error();
        return;
    }

    _receiver.segment_received(seg);

    // deliver ack to sender
    if (seg.header().ack) {
        _sender.ack_received(seg.header().ackno, seg.header().win);
    }

    // reset linger_after_streams_finish if remote EOF before inbound EOF
    if (_receiver.stream_out().eof() && !_sender.stream_in().eof()) {
        _linger_after_streams_finish = false;
    }

    bool shot = shot_segments();
    // Deal with 2nd+ FIN
    if (!shot && seg.header().fin) {
        _sender.send_empty_segment();
        shot_segments();
    }
}

bool TCPConnection::active() const {
    if (_receiver.stream_out().error() || _sender.stream_in().error()) {
        return false;
    }
    if (_receiver.stream_out().eof() && _sender.stream_in().eof() && _sender.bytes_in_flight() == 0 &&
        !new_ackno_to_be_sent() && !_linger_after_streams_finish) {
        return false;
    }
    return true;
}

size_t TCPConnection::write(const string &data) {
    size_t writen = _sender.stream_in().write(data);
    shot_segments();
    return writen;
}

//! \param[in] ms_since_last_tick number of milliseconds since the last call to this method
void TCPConnection::tick(const size_t ms_since_last_tick) {
    _sender.tick(ms_since_last_tick);
    _time_since_last_segment_received += ms_since_last_tick;
    if (_time_since_last_segment_received >= 10 * _cfg.rt_timeout) {
        _linger_after_streams_finish = false;
    }
    shot_segments();
}

void TCPConnection::end_input_stream() {
    _sender.stream_in().end_input();
    shot_segments();
}

void TCPConnection::connect() { shot_segments(); }

bool TCPConnection::shot_segments(bool fill_window) {
    bool shoot = false;
    if (fill_window) {
        _sender.fill_window();
    }
    while (active() && !_sender.segments_out().empty()) {
        auto seg = _sender.segments_out().front();
        // Add ackno to sending segs
        if (_receiver.ackno().has_value()) {
            seg.header().ack = true;
            seg.header().ackno = _receiver.ackno().value();
            _last_ackno_sent = _receiver.ackno();
        }
        _segments_out.push(seg);
        _sender.segments_out().pop();
        shoot = true;
        _sender.fill_window();
    }
    if (!shoot && new_ackno_to_be_sent()) {  // new ackno
        auto x = _receiver.ackno().value();
        DUMMY_CODE(x);
        _sender.send_empty_segment();
        shot_segments();
        shoot = true;
    }
    return shoot;
}

bool TCPConnection::new_ackno_to_be_sent() const {  // receiver's ack_no is changed and not sent yet.
    return _receiver.ackno().has_value() &&
           (!_last_ackno_sent.has_value() || (_last_ackno_sent.value() != _receiver.ackno().value()));
}

TCPConnection::~TCPConnection() {
    try {
        if (active()) {
            cerr << "Warning: Unclean shutdown of TCPConnection\n";

            // Your code here: need to send a RST segment to the peer
            TCPSegment segment;
            segment.header().seqno = _sender.next_seqno();
            segment.header().rst = true;
            _segments_out.push(segment);
        }
    } catch (const exception &e) {
        std::cerr << "Exception destructing TCP FSM: " << e.what() << std::endl;
    }
}
