"""Terminal colors of tracebacks and help texts (CPython's _colorize): the interpreter's
output is not colored unless FORCE_COLOR / PYTHON_COLORS=1 asks for it."""
import os
import sys

COLORIZE = True


class ANSIColors:
    RESET = "\x1b[0m"
    BLACK = "\x1b[30m"
    BLUE = "\x1b[34m"
    CYAN = "\x1b[36m"
    GREEN = "\x1b[32m"
    GREY = "\x1b[90m"
    MAGENTA = "\x1b[35m"
    RED = "\x1b[31m"
    WHITE = "\x1b[37m"
    YELLOW = "\x1b[33m"
    BOLD = "\x1b[1m"
    BOLD_BLUE = "\x1b[1;34m"
    BOLD_CYAN = "\x1b[1;36m"
    BOLD_GREEN = "\x1b[1;32m"
    BOLD_MAGENTA = "\x1b[1;35m"
    BOLD_RED = "\x1b[1;31m"
    BOLD_YELLOW = "\x1b[1;33m"
    INTENSE_BLUE = "\x1b[94m"
    INTENSE_CYAN = "\x1b[96m"
    INTENSE_GREEN = "\x1b[92m"
    INTENSE_MAGENTA = "\x1b[95m"
    INTENSE_RED = "\x1b[91m"
    INTENSE_YELLOW = "\x1b[93m"


class NoColors:
    pass


for _name in dir(ANSIColors):
    if not _name.startswith("__"):
        setattr(NoColors, _name, "")


class _Section:
    """A theme section: every color it is asked for ('' without colors)."""

    def __init__(self, name, colored):
        self._name = name
        self._colored = colored

    def __getattr__(self, attr):
        if attr.startswith("__"):
            raise AttributeError(attr)
        if not self._colored:
            return ""
        if attr == "reset":
            return ANSIColors.RESET
        return {"usage": ANSIColors.BOLD_BLUE, "prog": ANSIColors.BOLD_MAGENTA, "prog_extra": ANSIColors.MAGENTA,
                "heading": ANSIColors.BOLD_BLUE, "summary_long_option": ANSIColors.CYAN,
                "summary_short_option": ANSIColors.GREEN, "summary_label": ANSIColors.YELLOW,
                "summary_action": ANSIColors.GREEN, "long_option": ANSIColors.BOLD_CYAN,
                "short_option": ANSIColors.BOLD_GREEN, "label": ANSIColors.BOLD_YELLOW,
                "action": ANSIColors.BOLD_GREEN, "type": ANSIColors.BOLD_MAGENTA,
                "message": ANSIColors.MAGENTA, "filename": ANSIColors.MAGENTA, "line_no": ANSIColors.MAGENTA,
                "frame": ANSIColors.MAGENTA, "error_highlight": ANSIColors.BOLD_RED,
                "error_range": ANSIColors.RED}.get(attr, "")


class Theme:
    def __init__(self, colored):
        self.argparse = _Section("argparse", colored)
        self.syntax = _Section("syntax", colored)
        self.traceback = _Section("traceback", colored)
        self.unittest = _Section("unittest", colored)
        self.difflib = _Section("difflib", colored)

    def no_colors(self):
        return Theme(False)


default_theme = Theme(True)
theme_no_color = Theme(False)


def get_colors(colorize=False, *, file=None):
    if colorize or can_colorize(file=file):
        return ANSIColors()
    return NoColors


def decolor(text):
    """text without its ANSI color codes."""
    out = []
    i = 0
    while i < len(text):
        if text[i] == "\x1b" and text[i + 1:i + 2] == "[":
            j = i + 2
            while j < len(text) and text[j] not in "mK":
                j += 1
            i = j + 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def can_colorize(*, file=None):
    if not COLORIZE:
        return False
    if os.environ.get("PYTHON_COLORS") == "0" or os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("PYTHON_COLORS") == "1" or os.environ.get("FORCE_COLOR"):
        return True
    return False


def get_theme(*, tty_file=None, force_color=False, force_no_color=False):
    if force_color or (not force_no_color and can_colorize(file=tty_file)):
        return default_theme
    return theme_no_color


def set_theme(t):
    global default_theme
    default_theme = t
