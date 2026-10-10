"""Parser for command line options (CPython's getopt): getopt(args, shortopts, longopts),
gnu_getopt(...), GetoptError. Typed: options come back as (option, value) pairs of str."""
import os
from typing import TypeVar

__all__ = ["GetoptError", "error", "getopt", "gnu_getopt"]

_L = TypeVar("_L")


class GetoptError(Exception):
    """An unrecognized option, or a missing / unwanted argument: msg, opt."""

    def __init__(self, msg: str, opt: str = "") -> None:
        super().__init__(msg)
        self.msg = msg
        self.opt = opt

    def __str__(self) -> str:
        return self.msg


error = GetoptError


def _longs(longopts: _L) -> list[str]:
    if isinstance(longopts, str):
        return [longopts]
    else:
        return list(longopts)


def getopt(args: list[str], shortopts: str, longopts: _L = ()) -> tuple[list[tuple[str, str]], list[str]]:
    """(the options and their values, the arguments after them): options end at the first
    non-option argument (or at "--")."""
    opts: list[tuple[str, str]] = []
    lo = _longs(longopts)
    while args and args[0].startswith("-") and args[0] != "-":
        if args[0] == "--":
            args = args[1:]
            break
        if args[0].startswith("--"):
            args = _do_longs(opts, args[0][2:], lo, args[1:])
        else:
            args = _do_shorts(opts, args[0][1:], shortopts, args[1:])
    return opts, args


def gnu_getopt(args: list[str], shortopts: str, longopts: _L = ()) -> tuple[list[tuple[str, str]], list[str]]:
    """getopt(), but options and other arguments may be mixed ("+" first in shortopts, or
    POSIXLY_CORRECT: they end at the first other argument)."""
    opts: list[tuple[str, str]] = []
    prog_args: list[str] = []
    lo = _longs(longopts)
    all_options_first = False
    if shortopts.startswith("-"):
        shortopts = shortopts[1:]
    elif shortopts.startswith("+"):
        shortopts = shortopts[1:]
        all_options_first = True
    elif os.environ.get("POSIXLY_CORRECT"):
        all_options_first = True
    while args:
        if args[0] == "--":
            prog_args += args[1:]
            break
        if args[0][:2] == "--":
            args = _do_longs(opts, args[0][2:], lo, args[1:])
        elif args[0][:1] == "-" and args[0] != "-":
            args = _do_shorts(opts, args[0][1:], shortopts, args[1:])
        else:
            if all_options_first:
                prog_args += args
                break
            prog_args.append(args[0])
            args = args[1:]
    return opts, prog_args


def _do_longs(opts: list[tuple[str, str]], opt: str, longopts: list[str], args: list[str]) -> list[str]:
    optarg: str | None = None
    i = opt.find("=")
    if i >= 0:
        optarg = opt[i + 1:]
        opt = opt[:i]
    has_arg, opt = _long_has_args(opt, longopts)
    if has_arg != "":
        if optarg is None and has_arg != "?":
            if not args:
                raise GetoptError("option --" + opt + " requires argument", opt)
            optarg = args[0]
            args = args[1:]
    elif optarg is not None:
        raise GetoptError("option --" + opt + " must not have an argument", opt)
    opts.append(("--" + opt, optarg if optarg else ""))
    return args


def _long_has_args(opt: str, longopts: list[str]) -> tuple[str, str]:
    """("" no argument, "=" one, "?" an optional one; the option's whole name)"""
    possibilities = [o for o in longopts if o.startswith(opt)]
    if not possibilities:
        raise GetoptError("option --" + opt + " not recognized", opt)
    if opt in possibilities:
        return "", opt
    elif opt + "=" in possibilities:
        return "=", opt
    elif opt + "=?" in possibilities:
        return "?", opt
    if len(possibilities) > 1:
        raise GetoptError("option --" + opt + " not a unique prefix; possible options: " + ", ".join(possibilities), opt)
    unique_match = possibilities[0]
    if unique_match.endswith("=?"):
        return "?", unique_match[:-2]
    if unique_match.endswith("="):
        return "=", unique_match[:-1]
    return "", unique_match


def _do_shorts(opts: list[tuple[str, str]], optstring: str, shortopts: str, args: list[str]) -> list[str]:
    while optstring != "":
        opt = optstring[0]
        optstring = optstring[1:]
        has_arg = _short_has_arg(opt, shortopts)
        if has_arg != "":
            if optstring == "" and has_arg != "?":
                if not args:
                    raise GetoptError("option -" + opt + " requires argument", opt)
                optstring = args[0]
                args = args[1:]
            optarg = optstring
            optstring = ""
        else:
            optarg = ""
        opts.append(("-" + opt, optarg))
    return args


def _short_has_arg(opt: str, shortopts: str) -> str:
    for i in range(len(shortopts)):
        if opt == shortopts[i] != ":":
            if not shortopts.startswith(":", i + 1):
                return ""
            if shortopts.startswith("::", i + 1):
                return "?"
            return "="
    raise GetoptError("option -" + opt + " not recognized", opt)
