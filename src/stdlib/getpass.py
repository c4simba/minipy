"""getpass(prompt): a line read (shown as typed here); getuser(): the user's name."""
import os
import sys

__all__ = ["getpass", "getuser", "GetPassWarning"]


class GetPassWarning(UserWarning):
    pass


def getpass(prompt: str = "Password: ", stream: None = None, *, echo_char: str | None = None) -> str:
    """The line typed after prompt (here it is shown as typed)."""
    return input(prompt)


def getuser() -> str:
    """The user's name: LOGNAME, USER, LNAME or USERNAME (KolibriOS: "user")."""
    for name in ("LOGNAME", "USER", "LNAME", "USERNAME"):
        user = os.environ.get(name)
        if user:
            return user
    if sys.platform == "kolibrios":
        return "user"
    raise OSError("No username set in the environment")
