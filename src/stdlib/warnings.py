"""Python part of the warnings subsystem (CPython's warnings): warn, filterwarnings,
simplefilter, resetwarnings, catch_warnings, showwarning, formatwarning, deprecated."""
import sys

if not sys._compiled:
    import sys
    import _thread

    __all__ = ["warn", "warn_explicit", "showwarning", "formatwarning", "filterwarnings", "simplefilter",
               "resetwarnings", "catch_warnings", "deprecated"]

    filters = []
    defaultaction = "default"
    onceregistry = {}
    _filters_version = 1
    _lock = _thread.allocate_lock()
    _registries = {}            # module name -> its registry (CPython: the module's __warningregistry__)


    def _filters_mutated_lock_held():
        global _filters_version
        _filters_version += 1


    def _filters_mutated():
        with _lock:
            _filters_mutated_lock_held()


    class WarningMessage(object):
        _WARNING_DETAILS = ("message", "category", "filename", "lineno", "file", "line", "source")

        def __init__(self, message, category, filename, lineno, file=None, line=None, source=None):
            self.message = message
            self.category = category
            self.filename = filename
            self.lineno = lineno
            self.file = file
            self.line = line
            self.source = source
            self._category_name = category.__name__ if category else None

        def __str__(self):
            return ("{message : %r, category : %r, filename : %r, lineno : %s, line : %r}" %
                    (self.message, self._category_name, self.filename, self.lineno, self.line))

        def __repr__(self):
            return f"<{type(self).__qualname__} {self}>"


    def showwarning(message, category, filename, lineno, file=None, line=None):
        """Hook to write a warning to a file; replace if you like."""
        msg = WarningMessage(message, category, filename, lineno, file, line)
        _showwarnmsg_impl(msg)


    def formatwarning(message, category, filename, lineno, line=None):
        """Function to format a warning the standard way."""
        msg = WarningMessage(message, category, filename, lineno, None, line)
        return _formatwarnmsg_impl(msg)


    def _showwarnmsg_impl(msg):
        file = msg.file
        if file is None:
            file = sys.stderr
            if file is None:
                return
        text = _formatwarnmsg(msg)
        try:
            file.write(text)
        except OSError:
            pass


    def _formatwarnmsg_impl(msg):
        category = msg.category.__name__
        s = f"{msg.filename}:{msg.lineno}: {category}: {msg.message}\n"
        if msg.line is None:
            try:
                import linecache
                line = linecache.getline(msg.filename, msg.lineno)
            except Exception:
                line = None
        else:
            line = msg.line
        if line:
            line = line.strip()
            s += "  %s\n" % line
        return s


    _showwarning_orig = showwarning


    def _showwarnmsg(msg):
        sw = showwarning
        if sw is not _showwarning_orig:
            if not callable(sw):
                raise TypeError("warnings.showwarning() must be set to a function or method")
            sw(msg.message, msg.category, msg.filename, msg.lineno, msg.file, msg.line)
            return
        _showwarnmsg_impl(msg)


    _formatwarning_orig = formatwarning


    def _formatwarnmsg(msg):
        fw = formatwarning
        if fw is not _formatwarning_orig:
            return fw(msg.message, msg.category, msg.filename, msg.lineno, msg.line)
        return _formatwarnmsg_impl(msg)


    _ACTIONS = {"error", "ignore", "always", "all", "default", "module", "once"}


    def filterwarnings(action, message="", category=Warning, module="", lineno=0, append=False):
        """Inserts an entry into the list of warnings filters (at the front)."""
        if action not in _ACTIONS:
            raise ValueError(f"invalid action: {action!r}")
        if not isinstance(message, str):
            raise TypeError("message must be a string")
        if not isinstance(category, type) or not issubclass(category, Warning):
            raise TypeError("category must be a Warning subclass")
        if not isinstance(module, str):
            raise TypeError("module must be a string")
        if not isinstance(lineno, int):
            raise TypeError("lineno must be an int")
        if lineno < 0:
            raise ValueError("lineno must be an int >= 0")
        if message or module:
            import re
        message = re.compile(message, re.I) if message else None
        module = re.compile(module) if module else None
        _add_filter(action, message, category, module, lineno, append=append)


    def simplefilter(action, category=Warning, lineno=0, append=False):
        """Inserts a simple entry (all modules and messages) into the list of warnings filters."""
        if action not in _ACTIONS:
            raise ValueError(f"invalid action: {action!r}")
        if not isinstance(lineno, int):
            raise TypeError("lineno must be an int")
        if lineno < 0:
            raise ValueError("lineno must be an int >= 0")
        _add_filter(action, None, category, None, lineno, append=append)


    def _add_filter(*item, append):
        with _lock:
            if not append:
                try:
                    filters.remove(item)
                except ValueError:
                    pass
                filters.insert(0, item)
            else:
                if item not in filters:
                    filters.append(item)
            _filters_mutated_lock_held()


    def resetwarnings():
        """Clears the list of warning filters, so that no filters are active."""
        with _lock:
            del filters[:]
            _filters_mutated_lock_held()


    class _OptionError(Exception):
        """Exception used by option processing helpers."""


    def _getaction(action):
        if not action:
            return "default"
        for a in ("default", "always", "all", "ignore", "module", "once", "error"):
            if a.startswith(action):
                return a
        raise _OptionError("invalid action: %r" % (action,))


    def _matches(pattern, text):
        if pattern is None:
            return True
        if isinstance(pattern, str):
            return pattern == text
        return pattern.match(text)


    def warn(message, category=None, stacklevel=1, source=None, *, skip_file_prefixes=()):
        """Issues a warning, or maybe ignores it or raises an exception."""
        if isinstance(message, Warning):
            category = message.__class__
        if category is None:
            category = UserWarning
        if not (isinstance(category, type) and issubclass(category, Warning)):
            raise TypeError("category must be a Warning subclass, not '{:s}'".format(type(category).__name__))
        if not isinstance(skip_file_prefixes, tuple):
            raise TypeError("skip_file_prefixes must be a tuple of strs.")
        if skip_file_prefixes:
            stacklevel = max(2, stacklevel)
        stack = sys._stack()                    # (outermost first; the last: warn itself)
        idx = len(stack) - 2
        if stacklevel > 1:
            for x in range(stacklevel - 1):
                idx -= 1
                while idx >= 0 and any(stack[idx][0].startswith(p) for p in skip_file_prefixes):
                    idx -= 1
        else:
            idx = len(stack) - 1 - stacklevel if stacklevel >= 0 else -1
        if idx < 0:
            filename = "<sys>"
            lineno = 0
            module = "sys"
        else:
            filename = stack[idx][0]
            lineno = stack[idx][1]
            module = sys._getframemodulename(len(stack) - 1 - idx) or "<string>"
        registry = _registries.setdefault(module, {})
        warn_explicit(message, category, filename, lineno, module, registry, None, source=source)


    def warn_explicit(message, category, filename, lineno, module=None, registry=None, module_globals=None, source=None):
        """Issues a warning for the given place (filename, lineno, module)."""
        lineno = int(lineno)
        if module is None:
            module = filename or "<unknown>"
            if module[-3:].lower() == ".py":
                module = module[:-3]
        if isinstance(message, Warning):
            text = str(message)
            category = message.__class__
        else:
            text = message
            message = category(message)
        key = (text, category, lineno)
        with _lock:
            if registry is None:
                registry = {}
            if registry.get("version", 0) != _filters_version:
                registry.clear()
                registry["version"] = _filters_version
            if registry.get(key):
                return
            item = None
            for item in filters:
                action, msg, cat, mod, ln = item
                if (_matches(msg, text) and issubclass(category, cat) and _matches(mod, module) and
                        (ln == 0 or lineno == ln)):
                    break
            else:
                action = defaultaction
            if action == "ignore":
                return
            if action == "error":
                pass
            elif action == "once":
                registry[key] = 1
                oncekey = (text, category)
                if onceregistry.get(oncekey):
                    return
                onceregistry[oncekey] = 1
            elif action in ("always", "all"):
                pass
            elif action == "module":
                registry[key] = 1
                altkey = (text, category, 0)
                if registry.get(altkey):
                    return
                registry[altkey] = 1
            elif action == "default":
                registry[key] = 1
            else:
                raise RuntimeError("Unrecognized action (%r) in warnings.filters:\n %s" % (action, item))
        if action == "error":
            raise message
        msg = WarningMessage(message, category, filename, lineno, source=source)
        _showwarnmsg(msg)


    class catch_warnings(object):
        """A context manager that saves and restores the warnings filters (and showwarning);
    record=True: the warnings go to the list it gives instead."""

        def __init__(self, *, record=False, module=None, action=None, category=Warning, lineno=0, append=False):
            self._record = record
            self._module = sys.modules["warnings"] if module is None else module
            self._entered = False
            if action is None:
                self._filter = None
            else:
                self._filter = (action, category, lineno, append)

        def __repr__(self):
            args = []
            if self._record:
                args.append("record=True")
            if self._module is not sys.modules["warnings"]:
                args.append("module=%r" % self._module)
            name = type(self).__name__
            return "%s(%s)" % (name, ", ".join(args))

        def __enter__(self):
            if self._entered:
                raise RuntimeError("Cannot enter %r twice" % self)
            self._entered = True
            with _lock:
                self._filters = self._module.filters
                self._module.filters = self._filters[:]
                self._showwarnmsg_impl = self._module._showwarnmsg_impl
                self._showwarning = self._module.showwarning
                self._module._filters_mutated_lock_held()
                if self._record:
                    log = []
                    self._module._showwarnmsg_impl = log.append
                    self._module.showwarning = self._module._showwarning_orig
                else:
                    log = None
            if self._filter is not None:
                self._module.simplefilter(*self._filter)
            return log

        def __exit__(self, *exc_info):
            if not self._entered:
                raise RuntimeError("Cannot exit %r without entering first" % self)
            with _lock:
                self._module.filters = self._filters
                self._module._showwarnmsg_impl = self._showwarnmsg_impl
                self._module.showwarning = self._showwarning
                self._module._filters_mutated_lock_held()


    class deprecated:
        """@deprecated("Use B instead"): using the class or function warns (DeprecationWarning)."""

        def __init__(self, message, /, *, category=DeprecationWarning, stacklevel=1):
            if not isinstance(message, str):
                raise TypeError(f"Expected an object of type str for 'message', not {type(message).__name__!r}")
            self.message = message
            self.category = category
            self.stacklevel = stacklevel

        def __call__(self, arg, /):
            msg = self.message
            category = self.category
            stacklevel = self.stacklevel
            if category is None:
                arg.__deprecated__ = msg
                return arg
            elif isinstance(arg, type):
                original_new = arg.__new__

                def __new__(cls, /, *args, **kwargs):
                    if cls is arg:
                        warn(msg, category=category, stacklevel=stacklevel + 1)
                    if original_new is not object.__new__:
                        return original_new(cls, *args, **kwargs)
                    elif cls.__init__ is object.__init__ and (args or kwargs):
                        raise TypeError(f"{cls.__name__}() takes no arguments")
                    else:
                        return original_new(cls)

                arg.__new__ = staticmethod(__new__)
                arg.__deprecated__ = __new__.__deprecated__ = msg
                return arg
            elif callable(arg):
                import functools

                @functools.wraps(arg)
                def wrapper(*args, **kwargs):
                    warn(msg, category=category, stacklevel=stacklevel + 1)
                    return arg(*args, **kwargs)

                arg.__deprecated__ = wrapper.__deprecated__ = msg
                return wrapper
            else:
                raise TypeError("@deprecated decorator with non-None category must be applied to "
                                f"a class or callable, not {arg!r}")


    _DEPRECATED_MSG = "{name!r} is deprecated and slated for removal in Python {remove}"


    def _deprecated(name, message=_DEPRECATED_MSG, *, remove, _version=sys.version_info):
        remove_formatted = f"{remove[0]}.{remove[1]}"
        if (_version[:2] > remove) or (_version[:2] == remove and _version[3] != "alpha"):
            msg = f"{name!r} was slated for removal after Python {remove_formatted} alpha"
            raise RuntimeError(msg)
        else:
            msg = message.format(name=name, remove=remove_formatted)
            warn(msg, DeprecationWarning, stacklevel=3)


    filters.append(("default", None, DeprecationWarning, "__main__", 0))
    filters.append(("ignore", None, DeprecationWarning, None, 0))
    filters.append(("ignore", None, PendingDeprecationWarning, None, 0))
    filters.append(("ignore", None, ImportWarning, None, 0))
    filters.append(("ignore", None, ResourceWarning, None, 0))


if sys._compiled:
    # Compiled programs: the same filters, registries and output. The warning's place is the
    # warn() call itself (stacklevel is ignored); showwarning / formatwarning are not
    # replaceable; catch_warnings() always gives a list (filled when record=True).
    import re as _re
    import linecache as _linecache
    from typing import TypeVar

    _M = TypeVar("_M")
    _S = TypeVar("_S")
    _G = TypeVar("_G")

    class WarningMessage:
        """One warning: the message (a Warning), its category, where it was."""

        def __init__(self, message: Warning, category: type[Warning], filename: str, lineno: int,
                     file: str | None = None, line: str | None = None) -> None:
            self.message = message
            self.category = category
            self.filename = filename
            self.lineno = lineno
            self.file = file
            self.line = line
            self._category_name = category.__name__
            self._src: str | None = None                 # (the source line of a warn() call)

        def __str__(self) -> str:
            return ("{message : " + repr(self.message) + ", category : " + repr(self._category_name) +
                    ", filename : " + repr(self.filename) + ", lineno : " + str(self.lineno) +
                    ", line : " + repr(self.line) + "}")

    class _Filter:
        def __init__(self, action: str, message: str, category: type[Warning], module: str, lineno: int) -> None:
            self.action = action
            self.message = message
            self.msg: _re.Pattern | None = _re.compile(message, _re.I) if message else None
            self.category = category
            self.module = module
            self.mod: _re.Pattern | None = _re.compile(module) if module else None
            self.lineno = lineno

        def same(self, o: "_Filter") -> bool:
            return (self.action == o.action and self.message == o.message and self.category is o.category
                    and self.module == o.module and self.lineno == o.lineno)

        def __repr__(self) -> str:
            return ("(" + repr(self.action) + ", " + (repr(self.msg) if self.message else "None") + ", <class '" +
                    self.category.__name__ + "'>, " + (repr(self.mod) if self.module else "None") + ", " +
                    str(self.lineno) + ")")

    filters: list[_Filter] = []
    defaultaction = "default"
    onceregistry: dict[str, int] = {}
    _registry: dict[str, int] = {}
    _recording: list[list[WarningMessage]] = []
    _ACTIONS = ("error", "ignore", "always", "all", "default", "module", "once")

    def _add_filter(f: _Filter, append: bool) -> None:
        if not append:
            for i in range(len(filters)):
                if filters[i].same(f):
                    del filters[i]
                    break
            filters.insert(0, f)
        else:
            for g in filters:
                if g.same(f):
                    return
            filters.append(f)
        _registry.clear()

    def filterwarnings(action: str, message: str = "", category: type[Warning] = Warning, module: str = "",
                       lineno: int = 0, append: bool = False) -> None:
        """A filter: what to do (action) with warnings whose text starts with message (a regular expression)
        of that category (or a subclass), from modules whose name matches module, at that line (0: any)."""
        if action not in _ACTIONS:
            raise ValueError("invalid action: " + repr(action))
        if lineno < 0:
            raise ValueError("lineno must be an int >= 0")
        _add_filter(_Filter(action, message, category, module, lineno), append)

    def simplefilter(action: str, category: type[Warning] = Warning, lineno: int = 0, append: bool = False) -> None:
        """A filter for every warning of that category."""
        if action not in _ACTIONS:
            raise ValueError("invalid action: " + repr(action))
        if lineno < 0:
            raise ValueError("lineno must be an int >= 0")
        _add_filter(_Filter(action, "", category, "", lineno), append)

    def resetwarnings() -> None:
        """No filters (every warning: the default action)."""
        del filters[:]
        _registry.clear()

    def formatwarning(message: _M, category: type[Warning], filename: str, lineno: int, line: str | None = None) -> str:
        """file:line: Category: message, and the line of the source."""
        s = filename + ":" + str(lineno) + ": " + category.__name__ + ": " + str(message) + "\n"
        if line is None:
            line = _linecache.getline(filename, lineno)
        if line:
            s += "  " + line.strip() + "\n"
        return s

    def showwarning(message: _M, category: type[Warning], filename: str, lineno: int, file: str | None = None,
                    line: str | None = None) -> None:
        """Writes the warning to sys.stderr."""
        print(formatwarning(message, category, filename, lineno, line), end="", file=sys.stderr)

    def _showwarnmsg(msg: WarningMessage) -> None:
        if len(_recording) > 0:
            _recording[-1].append(msg)
            return
        showwarning(msg.message, msg.category, msg.filename, msg.lineno, None, msg.line if msg.line is not None else msg._src)

    def _warn_impl(message: Warning, category: type[Warning], filename: str, lineno: int, module: str,
                   line: str | None) -> None:
        text = str(message)
        cname = category.__qualname__
        key = module + "\0" + text + "\0" + cname + "\0" + str(lineno)
        if key in _registry:
            return
        action = defaultaction
        for f in filters:
            m = f.msg
            md = f.mod
            if (m is None or m.match(text) is not None) and issubclass(category, f.category) and \
                    (md is None or md.match(module) is not None) and (f.lineno == 0 or lineno == f.lineno):
                action = f.action
                break
        if action == "ignore":
            return
        if action == "error":
            raise message
        if action == "once":
            _registry[key] = 1
            oncekey = text + "\0" + cname
            if oncekey in onceregistry:
                return
            onceregistry[oncekey] = 1
        elif action == "always" or action == "all":
            pass
        elif action == "module":
            _registry[key] = 1
            altkey = module + "\0" + text + "\0" + cname + "\0" + "0"
            if altkey in _registry:
                return
            _registry[altkey] = 1
        elif action == "default":
            _registry[key] = 1
        else:
            raise RuntimeError("Unrecognized action (" + repr(action) + ") in warnings.filters:\n " + repr(action))
        wm = WarningMessage(message, category, filename, lineno)
        wm._src = line
        _showwarnmsg(wm)

    def warn(message: _M, category: type[Warning] = UserWarning, stacklevel: int = 1, source: _S = None, *,
             skip_file_prefixes: _G = (), _file: str = sys._site("file"), _line: int = sys._site("line"),
             _mod: str = sys._site("name"), _src: str = sys._site("source")) -> None:
        """Issues a warning (a message of that category, or a Warning) as the filters say."""
        if isinstance(message, str):
            _warn_impl(category(message), category, _file, _line, _mod, _src)
        elif isinstance(message, Warning):
            _warn_impl(message, type(message), _file, _line, _mod, _src)
        else:
            _warn_impl(category(str(message)), category, _file, _line, _mod, _src)

    def warn_explicit(message: _M, category: type[Warning], filename: str, lineno: int, module: str | None = None,
                      registry: _G = None, module_globals: _G = None, source: _S = None) -> None:
        """Issues a warning as if from that file and line."""
        mod = module
        if mod is None:
            mod = filename if filename else "<unknown>"
            if mod[-3:].lower() == ".py":
                mod = mod[:-3]
        if isinstance(message, str):
            _warn_impl(category(message), category, filename, lineno, mod, None)
        elif isinstance(message, Warning):
            _warn_impl(message, type(message), filename, lineno, mod, None)
        else:
            _warn_impl(category(str(message)), category, filename, lineno, mod, None)

    class catch_warnings:
        """with catch_warnings(record=True) as w: the filters (and showing) as they were afterwards;
        w: the warnings issued inside (when record is true)."""

        def __init__(self, *, record: bool = False, module: str | None = None, action: str | None = None,
                     category: type[Warning] = Warning, lineno: int = 0, append: bool = False) -> None:
            self._record = record
            self._action = action
            self._category = category
            self._lineno = lineno
            self._append = append
            self._saved: list[_Filter] = []
            self._log: list[WarningMessage] = []
            self._entered = False

        def __enter__(self) -> list[WarningMessage]:
            if self._entered:
                raise RuntimeError("Cannot enter " + repr(self) + " twice")
            self._entered = True
            self._saved = filters[:]
            _registry.clear()
            a = self._action
            if a is not None:
                simplefilter(a, self._category, self._lineno, self._append)
            if self._record:
                _recording.append(self._log)
            return self._log

        def __exit__(self, t, v, tb) -> None:
            if not self._entered:
                raise RuntimeError("Cannot exit " + repr(self) + " without entering first")
            filters[:] = self._saved
            _registry.clear()
            if self._record and len(_recording) > 0:
                _recording.pop()

        def __repr__(self) -> str:
            args = []
            if self._record:
                args.append("record=True")
            return "catch_warnings(" + ", ".join(args) + ")"

    filterwarnings("default", category=DeprecationWarning, module="__main__", append=True)
    simplefilter("ignore", category=DeprecationWarning, append=True)
    simplefilter("ignore", category=PendingDeprecationWarning, append=True)
    simplefilter("ignore", category=ImportWarning, append=True)
    simplefilter("ignore", category=ResourceWarning, append=True)
