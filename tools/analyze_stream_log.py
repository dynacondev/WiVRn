#!/usr/bin/env python3
"""Summarize WiVRn macOS server logs and Android client logcat captures.

Server input: lines logged by wivrn-macos (heartbeat, NACK, fb):
  streamer: eye 0 encoded=60 idx=59 rel=1 sent=119 drop=0 snap=0.5ms enc=10.6ms
  link: NACK stream=1 frame=18, forcing IDR
  link: fb stream=0 frame=1 recv=.../.. todec=.. fromdec=.. blit=.. displ=..

Client input: logcat lines from the headset app:
  frame 18 was not sent with 2 data shards, 1 missing
  Failed to find a common frame for all decoders, dumping available frames ...

Usage:
  tools/analyze_stream_log.py [--kind auto|server|client] FILE [FILE ...]

Prints one summary line per file plus an aggregate. Exit 0 on success,
2 when a file cannot be read or its kind cannot be determined.
"""

import argparse
import re
import sys

HEARTBEAT_RE = re.compile(
    r"streamer: eye (\d+) encoded=(\d+) idx=(\d+) rel=(\d+)"
    r" sent=(\d+) drop=(\d+) snap=([\d.]+)ms enc=([\d.]+)ms"
)
NACK_RE = re.compile(r"link: NACK stream=(\d+) frame=(\d+)")
FB_RE = re.compile(r"link: fb stream=(\d+) frame=(\d+) .*todec=(-?\d+)")
SERVER_MS_RE = re.compile(r"\[\s*(\d+)\]")
INCOMPLETE_RE = re.compile(
    r"frame (\d+) was not sent with (\d+) data shards, (?:at least )?(\d+) missing"
)
NO_SHARD_RE = re.compile(r"frame (\d+) was not sent because no shard was received")
NO_COMMON_RE = re.compile(r"Failed to find a common frame")

# NACKs chained by at most this many server-ms form one IDR burst.
BURST_GAP_MS = 250
# A burst this large (or larger) flags an IDR storm window.
STORM_BURST = 4


def detect_kind(text):
    """Guess server vs client from content; None when unrecognized."""
    if "streamer: eye" in text or "link: NACK" in text or "WIVRN_MACOS" in text:
        return "server"
    if "was not sent" in text or "common frame" in text:
        return "client"
    return None


def analyze_server(lines):
    """Aggregate heartbeat / NACK / fb counters from server log lines."""
    eyes = {}
    nacks = []  # (server_ms or None, stream, frame)
    completed = 0
    for line in lines:
        if m := HEARTBEAT_RE.search(line):
            eye = int(m.group(1))
            eyes[eye] = {
                "encoded": int(m.group(2)),
                "idx": int(m.group(3)),
                "rel": int(m.group(4)),
                "sent": int(m.group(5)),
                "drop": int(m.group(6)),
                "snap_ms": float(m.group(7)),
                "enc_ms": float(m.group(8)),
            }
            prev = eyes.get(("max_enc", eye), 0.0)
            eyes[("max_enc", eye)] = max(prev, float(m.group(8)))
        elif m := NACK_RE.search(line):
            ms = ms_match.group(1) if (ms_match := SERVER_MS_RE.search(line)) else None
            nacks.append((int(ms) if ms is not None else None, int(m.group(1)), int(m.group(2))))
        elif m := FB_RE.search(line):
            if int(m.group(3)) != 0:
                completed += 1
    bursts = burst_windows([ms for ms, _, _ in nacks if ms is not None])
    return {"eyes": eyes, "nacks": nacks, "completed": completed, "bursts": bursts}


def burst_windows(stamps):
    """Group millisecond stamps into bursts chained by BURST_GAP_MS."""
    bursts = []
    for ms in sorted(stamps):
        if bursts and ms - bursts[-1][-1] <= BURST_GAP_MS:
            bursts[-1].append(ms)
        else:
            bursts.append([ms])
    return bursts


def analyze_client(lines):
    """Aggregate incomplete-frame and no-common-frame counters from logcat."""
    incomplete = 0
    no_shard = 0
    missing_total = 0
    missing_max = 0
    no_common = 0
    for line in lines:
        if m := INCOMPLETE_RE.search(line):
            incomplete += 1
            missing = int(m.group(3))
            missing_total += missing
            missing_max = max(missing_max, missing)
        elif NO_SHARD_RE.search(line):
            no_shard += 1
        if NO_COMMON_RE.search(line):
            no_common += 1
    return {
        "incomplete": incomplete,
        "no_shard": no_shard,
        "missing_total": missing_total,
        "missing_max": missing_max,
        "no_common": no_common,
    }


def format_server(name, res):
    """One-line server summary: skew, NACK-rate, IDR-storm indicators."""
    eyes = {k: v for k, v in res["eyes"].items() if isinstance(k, int)}
    n_nack = len(res["nacks"])
    n_enc = sum(e["encoded"] for e in eyes.values())
    rate = 100.0 * n_nack / n_enc if n_enc else 0.0
    if len(eyes) >= 2:
        ordered = [eyes[k] for k in sorted(eyes)]
        skew = abs(ordered[0]["idx"] - ordered[1]["idx"])
        skew_txt = f"idx-skew {skew}"
    else:
        skew_txt = "idx-skew n/a (single eye)"
    enc_txt = "/".join(f"{eyes[k]['enc_ms']:.1f}" for k in sorted(eyes)) if eyes else "n/a"
    max_enc = max([res["eyes"].get(("max_enc", k), 0.0) for k in eyes] or [0.0])
    storms = sum(1 for b in res["bursts"] if len(b) >= STORM_BURST)
    biggest = max((len(b) for b in res["bursts"]), default=0)
    drops = max([e["drop"] for e in eyes.values()] or [0])
    rel_txt = "/".join(f"{eyes[k]['rel']}" for k in sorted(eyes)) if eyes else "n/a"
    return (
        f"{name}: encoded {n_enc} ({skew_txt}) enc {enc_txt}ms cur/{max_enc:.1f}ms max,"
        f" NACK {n_nack} ({rate:.1f}/100 enc), rel {rel_txt}, drop {drops},"
        f" IDR-storm windows {storms} (biggest burst {biggest})"
    )


def format_client(name, res):
    """One-line client summary: loss counters and decoder-stall indicator."""
    avg = res["missing_total"] / res["incomplete"] if res["incomplete"] else 0.0
    return (
        f"{name}: incomplete {res['incomplete']} frames"
        f" (missing {avg:.1f} avg/{res['missing_max']} max, {res['no_shard']} zero-shard),"
        f" no-common-frame {res['no_common']}"
    )


def main(argv=None):
    """Parse files and print per-file plus aggregate summaries."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("auto", "server", "client"), default="auto")
    parser.add_argument("files", metavar="FILE", nargs="+")
    args = parser.parse_args(argv)

    tot_enc = tot_nack = tot_incomplete = tot_no_common = 0
    ok = True
    for path in args.files:
        try:
            with open(path, errors="replace") as f:
                text = f.read()
        except OSError as e:
            print(f"{path}: unreadable ({e.strerror or e})", file=sys.stderr)
            ok = False
            continue
        kind = args.kind if args.kind != "auto" else detect_kind(text)
        if kind == "server":
            res = analyze_server(text.splitlines())
            print(format_server(path, res))
            tot_enc += sum(e["encoded"] for e in res["eyes"].values() if isinstance(e, dict))
            tot_nack += len(res["nacks"])
        elif kind == "client":
            res = analyze_client(text.splitlines())
            print(format_client(path, res))
            tot_incomplete += res["incomplete"]
            tot_no_common += res["no_common"]
        else:
            print(f"{path}: unknown log kind (use --kind server|client)", file=sys.stderr)
            ok = False
    if len(args.files) > 1 and ok:
        print(
            f"total: server encoded {tot_enc} with {tot_nack} NACKs"
            f" ({100.0 * tot_nack / tot_enc:.1f}/100 enc);"
            f" client incomplete {tot_incomplete}, no-common-frame {tot_no_common}"
        )
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
