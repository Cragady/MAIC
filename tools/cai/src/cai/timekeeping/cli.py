"""cai time -- stamps and windows, in UTC, computed rather than typed.

**The failure this exists for, 2026-09-01:** a stamp written as `20:20:00Z` when
the local clock read 20:20 **MDT** — six hours wrong, in the field the entire
staleness measure rests on. It was found by a freeze check comparing the stamp
against git's own record, which is to say it was found by luck of having built a
second witness that same hour.

**Nobody should be converting local time to UTC by hand.** Every grant, freeze
stamp and boundary in this suite is a UTC instant, and every one of them was
being typed.
"""
import argparse
import datetime
import json
import re
import sys

UTC = datetime.timezone.utc
DUR = re.compile(r"^(\d+(?:\.\d+)?)\s*(h|hours?|m|min|minutes?|d|days?)$", re.I)


def now():
    return datetime.datetime.now(UTC)


def stamp(dt=None):
    return (dt or now()).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_duration(text):
    """`2h`, `90m`, `3 days` -> a timedelta. Refuses anything it cannot read."""
    m = DUR.match(text.strip())
    if not m:
        raise ValueError("cannot read %r as a duration -- try 2h, 90m, 3d" % text)
    n, unit = float(m.group(1)), m.group(2).lower()
    if unit.startswith("h"):
        return datetime.timedelta(hours=n)
    if unit.startswith("d"):
        return datetime.timedelta(days=n)
    return datetime.timedelta(minutes=n)


def window(duration, start=None):
    """A grant window: begins now (or `start`), ends after `duration`.

    Returns both instants in UTC **and** their local renderings, because the
    operator states a window in local time and the record keeps it in UTC. Both
    are shown so the two can be checked against each other rather than one being
    converted in someone's head.
    """
    s = start or now()
    e = s + parse_duration(duration)
    return {"granted": stamp(s), "expires": stamp(e),
            "granted_local": s.astimezone().strftime("%Y-%m-%d %H:%M %Z"),
            "expires_local": e.astimezone().strftime("%Y-%m-%d %H:%M %Z"),
            "duration": duration,
            "note": ("UTC is what the record keeps; the local rendering is shown so the "
                     "two can be checked against each other rather than converted by "
                     "hand. A stamp written as local time labelled Z is six hours wrong "
                     "here and has happened.")}


MAN_HELP = """cai time -- full reference

  cai time now                    the current instant, UTC and local
  cai time window 2h              a window starting now
  cai time until <UTC>            how long remains

WHY IT EXISTS
  A stamp was written as 20:20:00Z when the local clock read 20:20 MDT -- six
  hours wrong, in the field the staleness measure rests on. It was caught by a
  freeze check comparing the stamp against git's own record, which is to say it
  was caught by having built a second witness that same hour.

  Every grant, freeze stamp and boundary in this suite is a UTC instant, and
  every one of them was being typed by hand.

WHAT IT SHOWS
  Both renderings, always. The operator states a window in local time and the
  record keeps it in UTC, so both are printed and can be checked against each
  other rather than one being converted in someone's head.

EXIT   0 ok   2 usage
"""


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai time", add_help=False)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("now")
    w = sub.add_parser("window")
    w.add_argument("duration")
    u = sub.add_parser("until")
    u.add_argument("instant")
    args = ap.parse_args(argv)

    if args.cmd == "now":
        n = now()
        print(json.dumps({"utc": stamp(n),
                          "local": n.astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")},
                         ensure_ascii=False, indent=2))
        return 0
    if args.cmd == "window":
        try:
            print(json.dumps(window(args.duration), ensure_ascii=False, indent=2))
        except ValueError as e:
            sys.stderr.write("error: %s\n" % e)
            return 2
        return 0
    if args.cmd == "until":
        try:
            target = datetime.datetime.strptime(
                args.instant.replace("Z", ""), "%Y-%m-%dT%H:%M:%S").replace(tzinfo=UTC)
        except ValueError as e:
            sys.stderr.write("error: %s\n" % e)
            return 2
        delta = (target - now()).total_seconds()
        print(json.dumps({"instant": stamp(target), "seconds": int(delta),
                          "minutes": round(delta / 60, 1),
                          "expired": delta < 0,
                          "note": ("negative means it has passed" if delta < 0
                                   else "positive means it is still ahead")},
                         ensure_ascii=False, indent=2))
        return 0
    print(MAN_HELP)
    return 2


if __name__ == "__main__":
    sys.exit(main())
