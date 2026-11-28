#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include "log_writer.h"
#include "mabur/cal_wire.h"

namespace maburgs {

struct WallReport;
struct CapCell;
struct CapResult;

// The record of a calibration run. Writes `cal.log` inside the session
// directory (DebugSession::dir()) via a private LogWriter -- unlike
// ctl.log/probe.log/au.log this does not share main.cpp's one session
// writer, because a calibration run is rare and short-lived (a handful of
// sweeps, not a per-frame hot path), so every caller that wants one just
// constructs it from a directory. cal.log plus the `maburcal report`
// reader is how a run is examined after the fact.
//
// Record format is LOCKED (gs/bundle/maburcal's reader and
// tests/test_cal_log.cpp depend on the exact byte layout):
//
//   callog 6                                           # once, first line
//   R <nonce> <margin_db> <ch> <pick> <ldpc 0|1|->     # once per calibration run;
//                                                       # `-` when no ack announced
//                                                       # the stamp flags first
//   S <seq> <wall|capacity> <bw> <param>               # once per sweep the drone
//                                                       # accepted; param is the
//                                                       # measured channel for a
//                                                       # wall sweep, the dwell in
//                                                       # ms for capacity. Scopes
//                                                       # every P row below it to
//                                                       # that sweep's width, until
//                                                       # the next S row
//   W <bw> <ch> <anchor> <floor_evm_x10> <w0> .. <w7>  # one per finished wall
//                                                       # sweep, as the drone
//                                                       # reported it
//   X <seq> <token> <reason>                           # a sweep the drone could
//                                                       # not run (X 0 power20
//                                                       # unsupported), or a wall
//                                                       # sweep on a channel it
//                                                       # cannot measure (unparked)
//   P <rate> <offered> <recv_best> <air_bits_best> <eff_x1000> <blocked_pct> <flags> <span_ms>
//                                                # one row per rate, one capacity
//                                                # sweep's result; span_ms is the
//                                                # best card's first-to-last
//                                                # receive span eff was timed
//                                                # over (flag 512, short_span:
//                                                # too short to trust, eff over
//                                                # the commanded dwell instead)
//   A <bw> <0|1> <reason to end of line>          # one row per apply attempt
//
// `pick` is one of auto|pinned|onecard|stayed -- the channel selection
// outcome this run's R line was raised under; the string is opaque to this
// class, which writes whatever the caller hands it verbatim. margin_db
// prints to two decimals.
//
// A W row's walls are SIGNED and RELATIVE to its own `anchor`, the drone
// chip's TXAGC reference on that channel; the floor EVM is in 0.1 dB.
//
// The marker and the per-run parameters are deliberately split: `callog N`
// versions the FILE (mirrors ctllog N elsewhere in this codebase) and is
// written at most once no matter how many runs share the session
// directory, while `R` carries one run's own parameters and is written
// once at the start of EVERY run. Nothing about a run rotates the session,
// so a retry puts a second run's records in the same file; an R line is
// what a reader keys runs by, and it marks where one run's lines end and
// the next begins.
//
// A capacity sweep's P row never carries a tx_bound point as a written
// value -- the flags field is the only place that distinction shows up in
// this file; writing (or not writing) the row at all is the caller's
// decision, not this class's.
//
// Per the project's compatibility policy the `callog N` marker is what
// versions this file -- the schema itself is free to change, provided both
// in-repo consumers (gs/bundle/maburcal and this test) change in the same
// commit. No migration shim, no reserved bytes.
//
// Every failure mode (LogWriter::open() unable to prepare the file, ...) is
// non-fatal: ok() reads false and every record method becomes a silent
// no-op. maburgs must never exit or crash over this log.
// True when <dir>/cal.log does not exist yet -- i.e. the CalLog about to be
// constructed on `dir` will be that FILE's first writer and header() is due.
// Ask this BEFORE constructing the CalLog: the constructor opens the file for
// append and so creates it, after which the answer is always false.
//
// This exists so the once-per-file rule is decided by the file, and never
// inferred from something merely correlated with it. maburgs originally
// inferred it from DebugSession::rejoined() -- "did this process allocate the
// session directory or adopt an existing one" -- which is a different
// question with a different answer: restarting maburgs rejoins the live
// session directory, and if that directory had never held a calibration, the
// first run wrote a headerless cal.log that gs/bundle/maburcal then refused
// to read at all. Seen on hardware, on the most ordinary path
// there is (deploy a binary, then calibrate).
bool cal_log_header_due(const std::string& dir);

// Call once before constructing the CalLog: returns whether header() is due,
// and first retires a cal.log written in an OLDER format by renaming it to
// cal.log.callog<N>.
//
// The once-per-file marker rule means one file holds exactly one format's
// rows, and nothing else enforces it -- appending today's rows under
// yesterday's marker produces a file that misreports its own shape, which is
// the one thing the marker exists to prevent. Retiring rather than deleting
// keeps the old run's data on the DVR readable as what it is (CLAUDE.md:
// recordings outlive the code that wrote them).
bool cal_log_prepare(const std::string& dir);

class CalLog {
 public:
  // dir is the session directory (DebugSession::dir()); the file is always
  // "cal.log" inside it, opened for append. A wrapper respawn that rejoins
  // an existing session directory must not call header() again -- the
  // caller decides whether this construction is the FILE's first ever
  // writer (header()) or merely another run sharing the session (run()
  // only); this class never writes anything on its own.
  explicit CalLog(const std::string& dir);

  CalLog(const CalLog&) = delete;
  CalLog& operator=(const CalLog&) = delete;

  bool ok() const { return s_ != LogWriter::kBadStream; }

  // The format-marker line, `callog 6`. Call at most once per FILE (i.e.
  // never on a rejoin of an already-headed session directory) -- it
  // carries no per-run data, so unlike ctl.log's single combined header
  // there is no reason for a second calibration run in the same session to
  // repeat it.
  void header();
  // One run's own parameters: `R <nonce> <margin_db> <ch> <pick> <ldpc>`.
  // Call once at the start of EVERY calibration run, including the second
  // and later ones sharing a session directory -- this is what a reader
  // keys a run's lines by and what delimits one run's lines from the next.
  // `pick` is one of auto|pinned|onecard|stayed, written verbatim.
  // `ldpc` nullopt (no ack heard before the run's first record) writes `-`.
  void run(uint32_t nonce, double margin_db, uint8_t ch, const char* pick,
           std::optional<bool> ldpc);
  // One sweep's own parameters: `S <seq> <wall|capacity> <bw> <param>`,
  // param the measured channel (wall) or the dwell in ms (capacity).
  // Scopes every P row that follows, up to the next sweep() call, to this
  // sweep's width.
  void sweep(uint8_t seq, mabur::cal::Kind kind, uint8_t bw, uint32_t param);
  // One finished wall sweep: `W <bw> <ch> <anchor> <floor_evm_x10> <w0>
  // .. <w7>`.
  void wall(const WallReport& w);
  // A sweep the drone could not run: `X <seq> <token> <reason>`.
  void skipped(uint8_t seq, const std::string& token, const char* reason);
  // One capacity dwell's result: `P <rate> <offered> <recv_best>
  // <air_bits_best> <eff_x1000> <blocked_pct> <flags> <span_ms>`. recv_best and
  // air_bits_best are the best card's own columns from `cell` (CapResult's
  // best_card), not a union across cards.
  void cap(uint8_t rate, const CapCell& c, const CapResult& r);
  // One apply attempt's outcome: `A <bw> <0|1> <reason to end of line>`.
  // `reason` keeps its spaces -- this is the last field on the line, so
  // nothing after it needs delimiting.
  void applied(uint8_t bw, bool ok, const std::string& reason);

  // Blocks until every record handed over so far is on disk. Call at the
  // end of a run (Done or Failed), and only there: LogWriter's own thread
  // flushes at 1 Hz, which is fine for the continuous logs but not for
  // this one -- `maburcal start` polls `status` every 500 ms and reads
  // cal.log the moment it sees state=done, so the records written at the
  // run's last transition could still be sitting in the stdio buffer and
  // the report render from a file missing its tail. The producer knows
  // when a run is over; the reader can only guess. Producer-thread only, like LogWriter::open().
  void flush();

 private:
  LogWriter w_;
  LogWriter::Stream s_;
};

}  // namespace maburgs
