"""Configuration file parser.

A configuration file consists of sections, lead by a "[section]" header,
and followed by "name: value" entries, with continuations and such in
the style of RFC 822.

Intrinsic defaults can be specified by passing them into the
ConfigParser constructor as a dictionary.

class:

ConfigParser -- responsible for parsing a list of
                    configuration files, and managing the parsed database.

    methods:

    __init__(defaults=None, dict_type=_default_dict, allow_no_value=False,
             delimiters=('=', ':'), comment_prefixes=('#', ';'),
             inline_comment_prefixes=None, strict=True,
             empty_lines_in_values=True, default_section='DEFAULT',
             interpolation=<unset>, converters=<unset>,
             allow_unnamed_section=False):
        Create the parser. When `defaults` is given, it is initialized into the
        dictionary or intrinsic defaults. The keys must be strings, the values
        must be appropriate for %()s string interpolation.

        When `dict_type` is given, it will be used to create the dictionary
        objects for the list of sections, for the options within a section, and
        for the default values.

        When `delimiters` is given, it will be used as the set of substrings
        that divide keys from values.

        When `comment_prefixes` is given, it will be used as the set of
        substrings that prefix comments in empty lines. Comments can be
        indented.

        When `inline_comment_prefixes` is given, it will be used as the set of
        substrings that prefix comments in non-empty lines.

        When `strict` is True, the parser won't allow for any section or option
        duplicates while reading from a single source (file, string or
        dictionary). Default is True.

        When `empty_lines_in_values` is False (default: True), each empty line
        marks the end of an option. Otherwise, internal empty lines of
        a multiline option are kept as part of the value.

        When `allow_no_value` is True (default: False), options without
        values are accepted; the value presented for these is None.

        When `default_section` is given, the name of the special section is
        named accordingly. By default it is called ``"DEFAULT"`` but this can
        be customized to point to any other valid section name. Its current
        value can be retrieved using the ``parser_instance.default_section``
        attribute and may be modified at runtime.

        When `interpolation` is given, it should be an Interpolation subclass
        instance. It will be used as the handler for option value
        pre-processing when using getters. RawConfigParser objects don't do
        any sort of interpolation, whereas ConfigParser uses an instance of
        BasicInterpolation. The library also provides a ``zc.buildout``
        inspired ExtendedInterpolation implementation.

        When `converters` is given, it should be a dictionary where each key
        represents the name of a type converter and each value is a callable
        implementing the conversion from string to the desired datatype. Every
        converter gets its corresponding get*() method on the parser object and
        section proxies.

        When `allow_unnamed_section` is True (default: False), options
        without section are accepted: the section for these is
        ``configparser.UNNAMED_SECTION``.

    sections()
        Return all the configuration section names, sans DEFAULT.

    has_section(section)
        Return whether the given section exists.

    has_option(section, option)
        Return whether the given option exists in the given section.

    options(section)
        Return list of configuration options for the named section.

    read(filenames, encoding=None)
        Read and parse the iterable of named configuration files, given by
        name.  A single filename is also allowed.  Non-existing files
        are ignored.  Return list of successfully read files.

    read_file(f, filename=None)
        Read and parse one configuration file, given as a file object.
        The filename defaults to f.name; it is only used in error
        messages (if f has no `name` attribute, the string `<???>` is used).

    read_string(string)
        Read configuration from a given string.

    read_dict(dictionary)
        Read configuration from a dictionary. Keys are section names,
        values are dictionaries with keys and values that should be present
        in the section. If the used dictionary type preserves order, sections
        and their keys will be added in order. Values are automatically
        converted to strings.

    get(section, option, raw=False, vars=None, fallback=_UNSET)
        Return a string value for the named option.  All % interpolations are
        expanded in the return values, based on the defaults passed into the
        constructor and the DEFAULT section.  Additional substitutions may be
        provided using the `vars` argument, which must be a dictionary whose
        contents override any pre-existing defaults. If `option` is a key in
        `vars`, the value from `vars` is used.

    getint(section, options, raw=False, vars=None, fallback=_UNSET)
        Like get(), but convert value to an integer.

    getfloat(section, options, raw=False, vars=None, fallback=_UNSET)
        Like get(), but convert value to a float.

    getboolean(section, options, raw=False, vars=None, fallback=_UNSET)
        Like get(), but convert value to a boolean (currently case
        insensitively defined as 0, false, no, off for False, and 1, true,
        yes, on for True).  Returns False or True.

    items(section=_UNSET, raw=False, vars=None)
        If section is given, return a list of tuples with (name, value) for
        each option in the section. Otherwise, return a list of tuples with
        (section_name, section_proxy) for each section, including DEFAULTSECT.

    remove_section(section)
        Remove the given file section and all its options.

    remove_option(section, option)
        Remove the given option from the given section.

    set(section, option, value)
        Set the given option.

    write(fp, space_around_delimiters=True)
        Write the configuration state in .ini format. If
        `space_around_delimiters` is True (the default), delimiters
        between keys and values are surrounded by spaces.
"""
import sys

if not sys._compiled:

    # Do not import dataclasses; overhead is unacceptable (gh-117703)

    from collections.abc import Iterable, MutableMapping
    from collections import ChainMap as _ChainMap
    import contextlib
    import functools
    import io
    import itertools
    import os
    import re
    import sys

    __all__ = ("NoSectionError", "DuplicateOptionError", "DuplicateSectionError",
               "NoOptionError", "InterpolationError", "InterpolationDepthError",
               "InterpolationMissingOptionError", "InterpolationSyntaxError",
               "ParsingError", "MissingSectionHeaderError",
               "MultilineContinuationError", "UnnamedSectionDisabledError",
               "InvalidWriteError", "ConfigParser", "RawConfigParser",
               "Interpolation", "BasicInterpolation",  "ExtendedInterpolation",
               "SectionProxy", "ConverterMapping",
               "DEFAULTSECT", "MAX_INTERPOLATION_DEPTH", "UNNAMED_SECTION")

    _default_dict = dict
    DEFAULTSECT = "DEFAULT"

    MAX_INTERPOLATION_DEPTH = 10



    # exception classes
    class Error(Exception):
        """Base class for ConfigParser exceptions."""

        def __init__(self, msg=''):
            self.message = msg
            Exception.__init__(self, msg)

        def __repr__(self):
            return self.message

        __str__ = __repr__


    class NoSectionError(Error):
        """Raised when no section matches a requested option."""

        def __init__(self, section):
            Error.__init__(self, 'No section: %r' % (section,))
            self.section = section
            self.args = (section, )


    class DuplicateSectionError(Error):
        """Raised when a section is repeated in an input source.

    Possible repetitions that raise this exception are: multiple creation
    using the API or in strict parsers when a section is found more than once
    in a single input file, string or dictionary.
    """

        def __init__(self, section, source=None, lineno=None):
            msg = [repr(section), " already exists"]
            if source is not None:
                message = ["While reading from ", repr(source)]
                if lineno is not None:
                    message.append(" [line {0:2d}]".format(lineno))
                message.append(": section ")
                message.extend(msg)
                msg = message
            else:
                msg.insert(0, "Section ")
            Error.__init__(self, "".join(msg))
            self.section = section
            self.source = source
            self.lineno = lineno
            self.args = (section, source, lineno)


    class DuplicateOptionError(Error):
        """Raised by strict parsers when an option is repeated in an input source.

    Current implementation raises this exception only when an option is found
    more than once in a single file, string or dictionary.
    """

        def __init__(self, section, option, source=None, lineno=None):
            msg = [repr(option), " in section ", repr(section),
                   " already exists"]
            if source is not None:
                message = ["While reading from ", repr(source)]
                if lineno is not None:
                    message.append(" [line {0:2d}]".format(lineno))
                message.append(": option ")
                message.extend(msg)
                msg = message
            else:
                msg.insert(0, "Option ")
            Error.__init__(self, "".join(msg))
            self.section = section
            self.option = option
            self.source = source
            self.lineno = lineno
            self.args = (section, option, source, lineno)


    class NoOptionError(Error):
        """A requested option was not found."""

        def __init__(self, option, section):
            Error.__init__(self, "No option %r in section: %r" %
                           (option, section))
            self.option = option
            self.section = section
            self.args = (option, section)


    class InterpolationError(Error):
        """Base class for interpolation-related exceptions."""

        def __init__(self, option, section, msg):
            Error.__init__(self, msg)
            self.option = option
            self.section = section
            self.args = (option, section, msg)


    class InterpolationMissingOptionError(InterpolationError):
        """A string substitution required a setting which was not available."""

        def __init__(self, option, section, rawval, reference):
            msg = ("Bad value substitution: option {!r} in section {!r} contains "
                   "an interpolation key {!r} which is not a valid option name. "
                   "Raw value: {!r}".format(option, section, reference, rawval))
            InterpolationError.__init__(self, option, section, msg)
            self.reference = reference
            self.args = (option, section, rawval, reference)


    class InterpolationSyntaxError(InterpolationError):
        """Raised when the source text contains invalid syntax.

    Current implementation raises this exception when the source text into
    which substitutions are made does not conform to the required syntax.
    """


    class InterpolationDepthError(InterpolationError):
        """Raised when substitutions are nested too deeply."""

        def __init__(self, option, section, rawval):
            msg = ("Recursion limit exceeded in value substitution: option {!r} "
                   "in section {!r} contains an interpolation key which "
                   "cannot be substituted in {} steps. Raw value: {!r}"
                   "".format(option, section, MAX_INTERPOLATION_DEPTH,
                             rawval))
            InterpolationError.__init__(self, option, section, msg)
            self.args = (option, section, rawval)


    class ParsingError(Error):
        """Raised when a configuration file does not follow legal syntax."""

        def __init__(self, source, *args):
            super().__init__(f'Source contains parsing errors: {source!r}')
            self.source = source
            self.errors = []
            self.args = (source, )
            if args:
                self.append(*args)

        def append(self, lineno, line):
            self.errors.append((lineno, line))
            self.message += f'\n\t[line {lineno:2d}]: {line!r}'

        def combine(self, others):
            messages = [self.message]
            for other in others:
                for lineno, line in other.errors:
                    self.errors.append((lineno, line))
                    messages.append(f'\n\t[line {lineno:2d}]: {line!r}')
            self.message = "".join(messages)
            return self

        @staticmethod
        def _raise_all(exceptions: Iterable['ParsingError']):
            """
        Combine any number of ParsingErrors into one and raise it.
        """
            exceptions = iter(exceptions)
            with contextlib.suppress(StopIteration):
                raise next(exceptions).combine(exceptions)



    class MissingSectionHeaderError(ParsingError):
        """Raised when a key-value pair is found before any section header."""

        def __init__(self, filename, lineno, line):
            Error.__init__(
                self,
                'File contains no section headers.\nfile: %r, line: %d\n%r' %
                (filename, lineno, line))
            self.source = filename
            self.lineno = lineno
            self.line = line
            self.args = (filename, lineno, line)


    class MultilineContinuationError(ParsingError):
        """Raised when a key without value is followed by continuation line"""
        def __init__(self, filename, lineno, line):
            Error.__init__(
                self,
                "Key without value continued with an indented line.\n"
                "file: %r, line: %d\n%r"
                %(filename, lineno, line))
            self.source = filename
            self.lineno = lineno
            self.line = line
            self.args = (filename, lineno, line)


    class UnnamedSectionDisabledError(Error):
        """Raised when an attempt to use UNNAMED_SECTION is made with the
    feature disabled."""
        def __init__(self):
            Error.__init__(self, "Support for UNNAMED_SECTION is disabled.")


    class _UnnamedSection:

        def __repr__(self):
            return "<UNNAMED_SECTION>"

    class InvalidWriteError(Error):
        """Raised when attempting to write data that the parser would read back differently.
    ex: writing a key which begins with the section header pattern would read back as a
    new section """

        def __init__(self, msg=''):
            Error.__init__(self, msg)


    UNNAMED_SECTION = _UnnamedSection()


    # Used in parser getters to indicate the default behaviour when a specific
    # option is not found it to raise an exception. Created to enable `None` as
    # a valid fallback value.
    _UNSET = object()


    class Interpolation:
        """Dummy interpolation that passes the value through with no changes."""

        def before_get(self, parser, section, option, value, defaults):
            return value

        def before_set(self, parser, section, option, value):
            return value

        def before_read(self, parser, section, option, value):
            return value

        def before_write(self, parser, section, option, value):
            return value


    class BasicInterpolation(Interpolation):
        """Interpolation as implemented in the classic ConfigParser.

    The option values can contain format strings which refer to other values in
    the same section, or values in the special default section.

    For example:

        something: %(dir)s/whatever

    would resolve the "%(dir)s" to the value of dir.  All reference
    expansions are done late, on demand. If a user needs to use a bare % in
    a configuration file, she can escape it by writing %%. Other % usage
    is considered a user error and raises `InterpolationSyntaxError`."""

        _KEYCRE = re.compile(r"%\(([^)]+)\)s")

        def before_get(self, parser, section, option, value, defaults):
            L = []
            self._interpolate_some(parser, option, L, value, section, defaults, 1)
            return ''.join(L)

        def before_set(self, parser, section, option, value):
            tmp_value = value.replace('%%', '') # escaped percent signs
            tmp_value = self._KEYCRE.sub('', tmp_value) # valid syntax
            if '%' in tmp_value:
                raise ValueError("invalid interpolation syntax in %r at "
                                 "position %d" % (value, tmp_value.find('%')))
            return value

        def _interpolate_some(self, parser, option, accum, rest, section, map,
                              depth):
            rawval = parser.get(section, option, raw=True, fallback=rest)
            if depth > MAX_INTERPOLATION_DEPTH:
                raise InterpolationDepthError(option, section, rawval)
            while rest:
                p = rest.find("%")
                if p < 0:
                    accum.append(rest)
                    return
                if p > 0:
                    accum.append(rest[:p])
                    rest = rest[p:]
                # p is no longer used
                c = rest[1:2]
                if c == "%":
                    accum.append("%")
                    rest = rest[2:]
                elif c == "(":
                    m = self._KEYCRE.match(rest)
                    if m is None:
                        raise InterpolationSyntaxError(option, section,
                            "bad interpolation variable reference %r" % rest)
                    var = parser.optionxform(m.group(1))
                    rest = rest[m.end():]
                    try:
                        v = map[var]
                    except KeyError:
                        raise InterpolationMissingOptionError(
                            option, section, rawval, var) from None
                    if "%" in v:
                        self._interpolate_some(parser, option, accum, v,
                                               section, map, depth + 1)
                    else:
                        accum.append(v)
                else:
                    raise InterpolationSyntaxError(
                        option, section,
                        "'%%' must be followed by '%%' or '(', "
                        "found: %r" % (rest,))


    class ExtendedInterpolation(Interpolation):
        """Advanced variant of interpolation, supports the syntax used by
    `zc.buildout`. Enables interpolation between sections."""

        _KEYCRE = re.compile(r"\$\{([^}]+)\}")

        def before_get(self, parser, section, option, value, defaults):
            L = []
            self._interpolate_some(parser, option, L, value, section, defaults, 1)
            return ''.join(L)

        def before_set(self, parser, section, option, value):
            tmp_value = value.replace('$$', '') # escaped dollar signs
            tmp_value = self._KEYCRE.sub('', tmp_value) # valid syntax
            if '$' in tmp_value:
                raise ValueError("invalid interpolation syntax in %r at "
                                 "position %d" % (value, tmp_value.find('$')))
            return value

        def _interpolate_some(self, parser, option, accum, rest, section, map,
                              depth):
            rawval = parser.get(section, option, raw=True, fallback=rest)
            if depth > MAX_INTERPOLATION_DEPTH:
                raise InterpolationDepthError(option, section, rawval)
            while rest:
                p = rest.find("$")
                if p < 0:
                    accum.append(rest)
                    return
                if p > 0:
                    accum.append(rest[:p])
                    rest = rest[p:]
                # p is no longer used
                c = rest[1:2]
                if c == "$":
                    accum.append("$")
                    rest = rest[2:]
                elif c == "{":
                    m = self._KEYCRE.match(rest)
                    if m is None:
                        raise InterpolationSyntaxError(option, section,
                            "bad interpolation variable reference %r" % rest)
                    path = m.group(1).split(':')
                    rest = rest[m.end():]
                    sect = section
                    opt = option
                    try:
                        if len(path) == 1:
                            opt = parser.optionxform(path[0])
                            v = map[opt]
                        elif len(path) == 2:
                            sect = path[0]
                            opt = parser.optionxform(path[1])
                            v = parser.get(sect, opt, raw=True)
                        else:
                            raise InterpolationSyntaxError(
                                option, section,
                                "More than one ':' found: %r" % (rest,))
                    except (KeyError, NoSectionError, NoOptionError):
                        raise InterpolationMissingOptionError(
                            option, section, rawval, ":".join(path)) from None
                    if v is None:
                        continue
                    if "$" in v:
                        self._interpolate_some(parser, opt, accum, v, sect,
                                               dict(parser.items(sect, raw=True)),
                                               depth + 1)
                    else:
                        accum.append(v)
                else:
                    raise InterpolationSyntaxError(
                        option, section,
                        "'$' must be followed by '$' or '{', "
                        "found: %r" % (rest,))


    class _ReadState:
        elements_added : set[str]
        cursect : dict[str, str] | None = None
        sectname : str | None = None
        optname : str | None = None
        lineno : int = 0
        indent_level : int = 0
        errors : list[ParsingError]

        def __init__(self):
            self.elements_added = set()
            self.errors = list()


    class _Line(str):
        __slots__ = 'clean', 'has_comments'

        def __new__(cls, val, *args, **kwargs):
            return super().__new__(cls, val)

        def __init__(self, val, comments):
            trimmed = val.strip()
            self.clean = comments.strip(trimmed)
            self.has_comments = trimmed != self.clean


    class _CommentSpec:
        def __init__(self, full_prefixes, inline_prefixes):
            full_patterns = (
                # prefix at the beginning of a line
                fr'^({re.escape(prefix)}).*'
                for prefix in full_prefixes
            )
            inline_patterns = (
                # prefix at the beginning of the line or following a space
                fr'(^|\s)({re.escape(prefix)}.*)'
                for prefix in inline_prefixes
            )
            self.pattern = re.compile('|'.join(itertools.chain(full_patterns, inline_patterns)))

        def strip(self, text):
            return self.pattern.sub('', text).rstrip()

        def wrap(self, text):
            return _Line(text, self)


    class RawConfigParser(MutableMapping):
        """ConfigParser that does not do interpolation."""

        # Regular expressions for parsing section headers and options
        _SECT_TMPL = r"""
        \[                                 # [
        (?P<header>.+)                     # very permissive!
        \]                                 # ]
        """
        _OPT_TMPL = r"""
        (?P<option>                        # very permissive!
            (?:(?!{delim})\S)*             # non-delimiter non-whitespace
            (?:\s+(?:(?!{delim})\S)+)*)    # optionally more words
        \s*(?P<vi>{delim})\s*              # any number of space/tab,
                                           # followed by any of the
                                           # allowed delimiters,
                                           # followed by any space/tab
        (?P<value>.*)$                     # everything up to eol
        """
        _OPT_NV_TMPL = r"""
        (?P<option>                        # very permissive!
            (?:(?!{delim})\S)*             # non-delimiter non-whitespace
            (?:\s+(?:(?!{delim})\S)+)*)    # optionally more words
        \s*(?:                             # any number of space/tab,
        (?P<vi>{delim})\s*                 # optionally followed by
                                           # any of the allowed
                                           # delimiters, followed by any
                                           # space/tab
        (?P<value>.*))?$                   # everything up to eol
        """
        # Interpolation algorithm to be used if the user does not specify another
        _DEFAULT_INTERPOLATION = Interpolation()
        # Compiled regular expression for matching sections
        SECTCRE = re.compile(_SECT_TMPL, re.VERBOSE)
        # Compiled regular expression for matching options with typical separators
        OPTCRE = re.compile(_OPT_TMPL.format(delim="=|:"), re.VERBOSE)
        # Compiled regular expression for matching options with optional values
        # delimited using typical separators
        OPTCRE_NV = re.compile(_OPT_NV_TMPL.format(delim="=|:"), re.VERBOSE)
        # Compiled regular expression for matching leading whitespace in a line
        NONSPACECRE = re.compile(r"\S")
        # Possible boolean values in the configuration.
        BOOLEAN_STATES = {'1': True, 'yes': True, 'true': True, 'on': True,
                          '0': False, 'no': False, 'false': False, 'off': False}

        def __init__(self, defaults=None, dict_type=_default_dict,
                     allow_no_value=False, *, delimiters=('=', ':'),
                     comment_prefixes=('#', ';'), inline_comment_prefixes=None,
                     strict=True, empty_lines_in_values=True,
                     default_section=DEFAULTSECT,
                     interpolation=_UNSET, converters=_UNSET,
                     allow_unnamed_section=False,):

            self._dict = dict_type
            self._sections = self._dict()
            self._defaults = self._dict()
            self._converters = ConverterMapping(self)
            self._proxies = self._dict()
            self._proxies[default_section] = SectionProxy(self, default_section)
            self._delimiters = tuple(delimiters)
            if delimiters == ('=', ':'):
                self._optcre = self.OPTCRE_NV if allow_no_value else self.OPTCRE
            else:
                d = "|".join(re.escape(d) for d in delimiters)
                if allow_no_value:
                    self._optcre = re.compile(self._OPT_NV_TMPL.format(delim=d),
                                              re.VERBOSE)
                else:
                    self._optcre = re.compile(self._OPT_TMPL.format(delim=d),
                                              re.VERBOSE)
            self._comments = _CommentSpec(comment_prefixes or (), inline_comment_prefixes or ())
            self._strict = strict
            self._allow_no_value = allow_no_value
            self._empty_lines_in_values = empty_lines_in_values
            self.default_section=default_section
            self._interpolation = interpolation
            if self._interpolation is _UNSET:
                self._interpolation = self._DEFAULT_INTERPOLATION
            if self._interpolation is None:
                self._interpolation = Interpolation()
            if not isinstance(self._interpolation, Interpolation):
                raise TypeError(
                    f"interpolation= must be None or an instance of Interpolation;"
                    f" got an object of type {type(self._interpolation)}"
                )
            if converters is not _UNSET:
                self._converters.update(converters)
            if defaults:
                self._read_defaults(defaults)
            self._allow_unnamed_section = allow_unnamed_section

        def defaults(self):
            return self._defaults

        def sections(self):
            """Return a list of section names, excluding [DEFAULT]"""
            # self._sections will never have [DEFAULT] in it
            return list(self._sections.keys())

        def add_section(self, section):
            """Create a new section in the configuration.

        Raise DuplicateSectionError if a section by the specified name
        already exists. Raise ValueError if name is DEFAULT.
        """
            if section == self.default_section:
                raise ValueError('Invalid section name: %r' % section)

            if section is UNNAMED_SECTION:
                if not self._allow_unnamed_section:
                    raise UnnamedSectionDisabledError

            if section in self._sections:
                raise DuplicateSectionError(section)
            self._sections[section] = self._dict()
            self._proxies[section] = SectionProxy(self, section)

        def has_section(self, section):
            """Indicate whether the named section is present in the configuration.

        The DEFAULT section is not acknowledged.
        """
            return section in self._sections

        def options(self, section):
            """Return a list of option names for the given section name."""
            try:
                opts = self._sections[section].copy()
            except KeyError:
                raise NoSectionError(section) from None
            opts.update(self._defaults)
            return list(opts.keys())

        def read(self, filenames, encoding=None):
            """Read and parse a filename or an iterable of filenames.

        Files that cannot be opened are silently ignored; this is
        designed so that you can specify an iterable of potential
        configuration file locations (e.g. current directory, user's
        home directory, systemwide directory), and all existing
        configuration files in the iterable will be read.  A single
        filename may also be given.

        Return list of successfully read files.
        """
            if isinstance(filenames, (str, bytes, os.PathLike)):
                filenames = [filenames]
            encoding = io.text_encoding(encoding)
            read_ok = []
            for filename in filenames:
                try:
                    with open(filename, encoding=encoding) as fp:
                        self._read(fp, filename)
                except OSError:
                    continue
                if isinstance(filename, os.PathLike):
                    filename = os.fspath(filename)
                read_ok.append(filename)
            return read_ok

        def read_file(self, f, source=None):
            """Like read() but the argument must be a file-like object.

        The `f` argument must be iterable, returning one line at a time.
        Optional second argument is the `source` specifying the name of the
        file being read. If not given, it is taken from f.name. If `f` has no
        `name` attribute, `<???>` is used.
        """
            if source is None:
                try:
                    source = f.name
                except AttributeError:
                    source = '<???>'
            self._read(f, source)

        def read_string(self, string, source='<string>'):
            """Read configuration from a given string."""
            sfile = io.StringIO(string)
            self.read_file(sfile, source)

        def read_dict(self, dictionary, source='<dict>'):
            """Read configuration from a dictionary.

        Keys are section names, values are dictionaries with keys and values
        that should be present in the section. If the used dictionary type
        preserves order, sections and their keys will be added in order.

        All types held in the dictionary are converted to strings during
        reading, including section names, option names and keys.

        Optional second argument is the `source` specifying the name of the
        dictionary being read.
        """
            elements_added = set()
            for section, keys in dictionary.items():
                if section is not UNNAMED_SECTION:
                    section = str(section)
                try:
                    self.add_section(section)
                except (DuplicateSectionError, ValueError):
                    if self._strict and section in elements_added:
                        raise
                elements_added.add(section)
                for key, value in keys.items():
                    key = self.optionxform(str(key))
                    if value is not None:
                        value = str(value)
                    if self._strict and (section, key) in elements_added:
                        raise DuplicateOptionError(section, key, source)
                    elements_added.add((section, key))
                    self.set(section, key, value)

        def get(self, section, option, *, raw=False, vars=None, fallback=_UNSET):
            """Get an option value for a given section.

        If `vars` is provided, it must be a dictionary. The option is looked up
        in `vars` (if provided), `section`, and in `DEFAULTSECT` in that order.
        If the key is not found and `fallback` is provided, it is used as
        a fallback value. `None` can be provided as a `fallback` value.

        If interpolation is enabled and the optional argument `raw` is False,
        all interpolations are expanded in the return values.

        Arguments `raw`, `vars`, and `fallback` are keyword only.

        The section DEFAULT is special.
        """
            try:
                d = self._unify_values(section, vars)
            except NoSectionError:
                if fallback is _UNSET:
                    raise
                else:
                    return fallback
            option = self.optionxform(option)
            try:
                value = d[option]
            except KeyError:
                if fallback is _UNSET:
                    raise NoOptionError(option, section)
                else:
                    return fallback

            if raw or value is None:
                return value
            else:
                return self._interpolation.before_get(self, section, option, value,
                                                      d)

        def _get(self, section, conv, option, **kwargs):
            return conv(self.get(section, option, **kwargs))

        def _get_conv(self, section, option, conv, *, raw=False, vars=None,
                      fallback=_UNSET, **kwargs):
            try:
                return self._get(section, conv, option, raw=raw, vars=vars,
                                 **kwargs)
            except (NoSectionError, NoOptionError):
                if fallback is _UNSET:
                    raise
                return fallback

        # getint, getfloat and getboolean provided directly for backwards compat
        def getint(self, section, option, *, raw=False, vars=None,
                   fallback=_UNSET, **kwargs):
            return self._get_conv(section, option, int, raw=raw, vars=vars,
                                  fallback=fallback, **kwargs)

        def getfloat(self, section, option, *, raw=False, vars=None,
                     fallback=_UNSET, **kwargs):
            return self._get_conv(section, option, float, raw=raw, vars=vars,
                                  fallback=fallback, **kwargs)

        def getboolean(self, section, option, *, raw=False, vars=None,
                       fallback=_UNSET, **kwargs):
            return self._get_conv(section, option, self._convert_to_boolean,
                                  raw=raw, vars=vars, fallback=fallback, **kwargs)

        def items(self, section=_UNSET, raw=False, vars=None):
            """Return a list of (name, value) tuples for each option in a section.

        All % interpolations are expanded in the return values, based on the
        defaults passed into the constructor, unless the optional argument
        `raw` is true.  Additional substitutions may be provided using the
        `vars` argument, which must be a dictionary whose contents overrides
        any pre-existing defaults.

        The section DEFAULT is special.
        """
            if section is _UNSET:
                return super().items()
            d = self._defaults.copy()
            try:
                d.update(self._sections[section])
            except KeyError:
                if section != self.default_section:
                    raise NoSectionError(section)
            orig_keys = list(d.keys())
            # Update with the entry specific variables
            if vars:
                for key, value in vars.items():
                    d[self.optionxform(key)] = value
            value_getter = lambda option: self._interpolation.before_get(self,
                section, option, d[option], d)
            if raw:
                value_getter = lambda option: d[option]
            return [(option, value_getter(option)) for option in orig_keys]

        def popitem(self):
            """Remove a section from the parser and return it as
        a (section_name, section_proxy) tuple. If no section is present, raise
        KeyError.

        The section DEFAULT is never returned because it cannot be removed.
        """
            for key in self.sections():
                value = self[key]
                del self[key]
                return key, value
            raise KeyError

        def optionxform(self, optionstr):
            return optionstr.lower()

        def has_option(self, section, option):
            """Check for the existence of a given option in a given section.
        If the specified `section` is None or an empty string, DEFAULT is
        assumed. If the specified `section` does not exist, returns False."""
            if not section or section == self.default_section:
                option = self.optionxform(option)
                return option in self._defaults
            elif section not in self._sections:
                return False
            else:
                option = self.optionxform(option)
                return (option in self._sections[section]
                        or option in self._defaults)

        def set(self, section, option, value=None):
            """Set an option."""
            if value:
                value = self._interpolation.before_set(self, section, option,
                                                       value)
            if not section or section == self.default_section:
                sectdict = self._defaults
            else:
                try:
                    sectdict = self._sections[section]
                except KeyError:
                    raise NoSectionError(section) from None
            sectdict[self.optionxform(option)] = value

        def write(self, fp, space_around_delimiters=True):
            """Write an .ini-format representation of the configuration state.

        If `space_around_delimiters` is True (the default), delimiters
        between keys and values are surrounded by spaces.

        Please note that comments in the original configuration file are not
        preserved when writing the configuration back.
        """
            if space_around_delimiters:
                d = " {} ".format(self._delimiters[0])
            else:
                d = self._delimiters[0]
            if self._defaults:
                self._write_section(fp, self.default_section,
                                        self._defaults.items(), d)
            if UNNAMED_SECTION in self._sections and self._sections[UNNAMED_SECTION]:
                self._write_section(fp, UNNAMED_SECTION, self._sections[UNNAMED_SECTION].items(), d, unnamed=True)

            for section in self._sections:
                if section is UNNAMED_SECTION:
                    continue
                self._write_section(fp, section,
                                    self._sections[section].items(), d)

        def _write_section(self, fp, section_name, section_items, delimiter, unnamed=False):
            """Write a single section to the specified 'fp'."""
            if not unnamed:
                fp.write("[{}]\n".format(section_name))
            for key, value in section_items:
                self._validate_key_contents(key)
                value = self._interpolation.before_write(self, section_name, key,
                                                         value)
                if value is not None or not self._allow_no_value:
                    value = delimiter + str(value).replace('\n', '\n\t')
                else:
                    value = ""
                fp.write("{}{}\n".format(key, value))
            fp.write("\n")

        def remove_option(self, section, option):
            """Remove an option."""
            if not section or section == self.default_section:
                sectdict = self._defaults
            else:
                try:
                    sectdict = self._sections[section]
                except KeyError:
                    raise NoSectionError(section) from None
            option = self.optionxform(option)
            existed = option in sectdict
            if existed:
                del sectdict[option]
            return existed

        def remove_section(self, section):
            """Remove a file section."""
            existed = section in self._sections
            if existed:
                del self._sections[section]
                del self._proxies[section]
            return existed

        def __getitem__(self, key):
            if key != self.default_section and not self.has_section(key):
                raise KeyError(key)
            return self._proxies[key]

        def __setitem__(self, key, value):
            # To conform with the mapping protocol, overwrites existing values in
            # the section.
            if key in self and self[key] is value:
                return
            # XXX this is not atomic if read_dict fails at any point. Then again,
            # no update method in configparser is atomic in this implementation.
            if key == self.default_section:
                self._defaults.clear()
            elif key in self._sections:
                self._sections[key].clear()
            self.read_dict({key: value})

        def __delitem__(self, key):
            if key == self.default_section:
                raise ValueError("Cannot remove the default section.")
            if not self.has_section(key):
                raise KeyError(key)
            self.remove_section(key)

        def __contains__(self, key):
            return key == self.default_section or self.has_section(key)

        def __len__(self):
            return len(self._sections) + 1 # the default section

        def __iter__(self):
            # XXX does it break when underlying container state changed?
            return itertools.chain((self.default_section,), self._sections.keys())

        def _read(self, fp, fpname):
            """Parse a sectioned configuration file.

        Each section in a configuration file contains a header, indicated by
        a name in square brackets (`[]`), plus key/value options, indicated by
        `name` and `value` delimited with a specific substring (`=` or `:` by
        default).

        Values can span multiple lines, as long as they are indented deeper
        than the first line of the value. Depending on the parser's mode, blank
        lines may be treated as parts of multiline values or ignored.

        Configuration files may include comments, prefixed by specific
        characters (`#` and `;` by default). Comments may appear on their own
        in an otherwise empty line or may be entered in lines holding values or
        section names. Please note that comments get stripped off when reading configuration files.
        """
            try:
                ParsingError._raise_all(self._read_inner(fp, fpname))
            finally:
                self._join_multiline_values()

        def _read_inner(self, fp, fpname):
            st = _ReadState()

            for st.lineno, line in enumerate(map(self._comments.wrap, fp), start=1):
                if not line.clean:
                    if self._empty_lines_in_values:
                        # add empty line to the value, but only if there was no
                        # comment on the line
                        if (not line.has_comments and
                            st.cursect is not None and
                            st.optname and
                            st.cursect[st.optname] is not None):
                            st.cursect[st.optname].append('') # newlines added at join
                    else:
                        # empty line marks end of value
                        st.indent_level = sys.maxsize
                    continue

                first_nonspace = self.NONSPACECRE.search(line)
                st.cur_indent_level = first_nonspace.start() if first_nonspace else 0

                if self._handle_continuation_line(st, line, fpname):
                    continue

                self._handle_rest(st, line, fpname)

            return st.errors

        def _handle_continuation_line(self, st, line, fpname):
            # continuation line?
            is_continue = (st.cursect is not None and st.optname and
                st.cur_indent_level > st.indent_level)
            if is_continue:
                if st.cursect[st.optname] is None:
                    raise MultilineContinuationError(fpname, st.lineno, line)
                st.cursect[st.optname].append(line.clean)
            return is_continue

        def _handle_rest(self, st, line, fpname):
            # a section header or option header?
            if self._allow_unnamed_section and st.cursect is None:
                self._handle_header(st, UNNAMED_SECTION, fpname)

            st.indent_level = st.cur_indent_level
            # is it a section header?
            mo = self.SECTCRE.match(line.clean)

            if not mo and st.cursect is None:
                raise MissingSectionHeaderError(fpname, st.lineno, line)

            self._handle_header(st, mo.group('header'), fpname) if mo else self._handle_option(st, line, fpname)

        def _handle_header(self, st, sectname, fpname):
            st.sectname = sectname
            if st.sectname in self._sections:
                if self._strict and st.sectname in st.elements_added:
                    raise DuplicateSectionError(st.sectname, fpname,
                                                st.lineno)
                st.cursect = self._sections[st.sectname]
                st.elements_added.add(st.sectname)
            elif st.sectname == self.default_section:
                st.cursect = self._defaults
            else:
                st.cursect = self._dict()
                self._sections[st.sectname] = st.cursect
                self._proxies[st.sectname] = SectionProxy(self, st.sectname)
                st.elements_added.add(st.sectname)
            # So sections can't start with a continuation line
            st.optname = None

        def _handle_option(self, st, line, fpname):
            # an option line?
            st.indent_level = st.cur_indent_level

            mo = self._optcre.match(line.clean)
            if not mo:
                # a non-fatal parsing error occurred. set up the
                # exception but keep going. the exception will be
                # raised at the end of the file and will contain a
                # list of all bogus lines
                st.errors.append(ParsingError(fpname, st.lineno, line))
                return

            st.optname, vi, optval = mo.group('option', 'vi', 'value')
            if not st.optname:
                st.errors.append(ParsingError(fpname, st.lineno, line))
            st.optname = self.optionxform(st.optname.rstrip())
            if (self._strict and
                (st.sectname, st.optname) in st.elements_added):
                raise DuplicateOptionError(st.sectname, st.optname,
                                        fpname, st.lineno)
            st.elements_added.add((st.sectname, st.optname))
            # This check is fine because the OPTCRE cannot
            # match if it would set optval to None
            if optval is not None:
                optval = optval.strip()
                st.cursect[st.optname] = [optval]
            else:
                # valueless option handling
                st.cursect[st.optname] = None

        def _join_multiline_values(self):
            defaults = self.default_section, self._defaults
            all_sections = itertools.chain((defaults,),
                                           self._sections.items())
            for section, options in all_sections:
                for name, val in options.items():
                    if isinstance(val, list):
                        val = '\n'.join(val).rstrip()
                    options[name] = self._interpolation.before_read(self,
                                                                    section,
                                                                    name, val)

        def _read_defaults(self, defaults):
            """Read the defaults passed in the initializer.
        Note: values can be non-string."""
            for key, value in defaults.items():
                self._defaults[self.optionxform(key)] = value

        def _unify_values(self, section, vars):
            """Create a sequence of lookups with 'vars' taking priority over
        the 'section' which takes priority over the DEFAULTSECT.

        """
            sectiondict = {}
            try:
                sectiondict = self._sections[section]
            except KeyError:
                if section != self.default_section:
                    raise NoSectionError(section) from None
            # Update with the entry specific variables
            vardict = {}
            if vars:
                for key, value in vars.items():
                    if value is not None:
                        value = str(value)
                    vardict[self.optionxform(key)] = value
            return _ChainMap(vardict, sectiondict, self._defaults)

        def _convert_to_boolean(self, value):
            """Return a boolean value translating from other types if necessary.
        """
            if value.lower() not in self.BOOLEAN_STATES:
                raise ValueError('Not a boolean: %s' % value)
            return self.BOOLEAN_STATES[value.lower()]

        def _validate_key_contents(self, key):
            """Raises an InvalidWriteError for any keys containing
        delimiters or that begins with the section header pattern"""
            if re.match(self.SECTCRE, key):
                raise InvalidWriteError(
                    f"Cannot write key {key}; begins with section pattern")
            for delim in self._delimiters:
                if delim in key:
                    raise InvalidWriteError(
                        f"Cannot write key {key}; contains delimiter {delim}")

        def _validate_value_types(self, *, section="", option="", value=""):
            """Raises a TypeError for illegal non-string values.

        Legal non-string values are UNNAMED_SECTION and falsey values if
        they are allowed.

        For compatibility reasons this method is not used in classic set()
        for RawConfigParsers. It is invoked in every case for mapping protocol
        access and in ConfigParser.set().
        """
            if section is UNNAMED_SECTION:
                if not self._allow_unnamed_section:
                    raise UnnamedSectionDisabledError
            elif not isinstance(section, str):
                raise TypeError("section names must be strings or UNNAMED_SECTION")
            if not isinstance(option, str):
                raise TypeError("option keys must be strings")
            if not self._allow_no_value or value:
                if not isinstance(value, str):
                    raise TypeError("option values must be strings")

        @property
        def converters(self):
            return self._converters


    class ConfigParser(RawConfigParser):
        """ConfigParser implementing interpolation."""

        _DEFAULT_INTERPOLATION = BasicInterpolation()

        def set(self, section, option, value=None):
            """Set an option.  Extends RawConfigParser.set by validating type and
        interpolation syntax on the value."""
            self._validate_value_types(option=option, value=value)
            super().set(section, option, value)

        def add_section(self, section):
            """Create a new section in the configuration.  Extends
        RawConfigParser.add_section by validating if the section name is
        a string."""
            self._validate_value_types(section=section)
            super().add_section(section)

        def _read_defaults(self, defaults):
            """Reads the defaults passed in the initializer, implicitly converting
        values to strings like the rest of the API.

        Does not perform interpolation for backwards compatibility.
        """
            try:
                hold_interpolation = self._interpolation
                self._interpolation = Interpolation()
                self.read_dict({self.default_section: defaults})
            finally:
                self._interpolation = hold_interpolation


    class SectionProxy(MutableMapping):
        """A proxy for a single section from a parser."""

        def __init__(self, parser, name):
            """Creates a view on a section of the specified `name` in `parser`."""
            self._parser = parser
            self._name = name
            for conv in parser.converters:
                key = 'get' + conv
                getter = functools.partial(self.get, _impl=getattr(parser, key))
                setattr(self, key, getter)

        def __repr__(self):
            return '<Section: {}>'.format(self._name)

        def __getitem__(self, key):
            if not self._parser.has_option(self._name, key):
                raise KeyError(key)
            return self._parser.get(self._name, key)

        def __setitem__(self, key, value):
            self._parser._validate_value_types(option=key, value=value)
            return self._parser.set(self._name, key, value)

        def __delitem__(self, key):
            if not (self._parser.has_option(self._name, key) and
                    self._parser.remove_option(self._name, key)):
                raise KeyError(key)

        def __contains__(self, key):
            return self._parser.has_option(self._name, key)

        def __len__(self):
            return len(self._options())

        def __iter__(self):
            return self._options().__iter__()

        def _options(self):
            if self._name != self._parser.default_section:
                return self._parser.options(self._name)
            else:
                return self._parser.defaults()

        @property
        def parser(self):
            # The parser object of the proxy is read-only.
            return self._parser

        @property
        def name(self):
            # The name of the section on a proxy is read-only.
            return self._name

        def get(self, option, fallback=None, *, raw=False, vars=None,
                _impl=None, **kwargs):
            """Get an option value.

        Unless `fallback` is provided, `None` will be returned if the option
        is not found.

        """
            # If `_impl` is provided, it should be a getter method on the parser
            # object that provides the desired type conversion.
            if not _impl:
                _impl = self._parser.get
            return _impl(self._name, option, raw=raw, vars=vars,
                         fallback=fallback, **kwargs)


    class ConverterMapping(MutableMapping):
        """Enables reuse of get*() methods between the parser and section proxies.

    If a parser class implements a getter directly, the value for the given
    key will be ``None``. The presence of the converter name here enables
    section proxies to find and use the implementation on the parser class.
    """

        GETTERCRE = re.compile(r"^get(?P<name>.+)$")

        def __init__(self, parser):
            self._parser = parser
            self._data = {}
            for getter in dir(self._parser):
                m = self.GETTERCRE.match(getter)
                if not m or not callable(getattr(self._parser, getter)):
                    continue
                self._data[m.group('name')] = None   # See class docstring.

        def __getitem__(self, key):
            return self._data[key]

        def __setitem__(self, key, value):
            try:
                k = 'get' + key
            except TypeError:
                raise ValueError('Incompatible key: {} (type: {})'
                                 ''.format(key, type(key)))
            if k == 'get':
                raise ValueError('Incompatible key: cannot use "" as a name')
            self._data[key] = value
            func = functools.partial(self._parser._get_conv, conv=value)
            func.converter = value
            setattr(self._parser, k, func)
            for proxy in self._parser.values():
                getter = functools.partial(proxy.get, _impl=func)
                setattr(proxy, k, getter)

        def __delitem__(self, key):
            try:
                k = 'get' + (key or None)
            except TypeError:
                raise KeyError(key)
            del self._data[key]
            for inst in itertools.chain((self._parser,), self._parser.values()):
                try:
                    delattr(inst, k)
                except AttributeError:
                    # don't raise since the entry was present in _data, silently
                    # clean up
                    continue

        def __iter__(self):
            return iter(self._data)

        def __len__(self):
            return len(self._data)


if sys._compiled:
    # Compiled programs: the same reading, interpolation (Basic, Extended), errors and writing;
    # values are str (or None: allow_no_value); converters= and custom dict types are not here.
    from typing import Callable, TypeVar

    _S = TypeVar("_S")
    _F = TypeVar("_F")
    _D = TypeVar("_D")
    _V = TypeVar("_V")

    DEFAULTSECT = "DEFAULT"
    MAX_INTERPOLATION_DEPTH = 10
    BOOLEAN_STATES = {'1': True, 'yes': True, 'true': True, 'on': True, '0': False, 'no': False, 'false': False, 'off': False}

    class Error(Exception):
        """Base class for ConfigParser exceptions."""

        def __init__(self, msg: str = '') -> None:
            super().__init__(msg)
            self.message = msg

    class NoSectionError(Error):
        """Raised when no section matches a requested option."""

        def __init__(self, section: str) -> None:
            Error.__init__(self, 'No section: ' + repr(section))
            self.section = section

    class DuplicateSectionError(Error):
        """Raised when a section is repeated in an input source."""

        def __init__(self, section: str, source: str | None = None, lineno: int | None = None) -> None:
            msg = repr(section) + " already exists"
            if source is not None:
                where = "While reading from " + repr(source)
                if lineno is not None:
                    where += " [line " + f"{lineno:2d}" + "]"
                msg = where + ": section " + msg
            else:
                msg = "Section " + msg
            Error.__init__(self, msg)
            self.section = section
            self.source = source
            self.lineno = lineno

    class DuplicateOptionError(Error):
        """Raised by strict parsers when an option is repeated in an input source."""

        def __init__(self, section: str, option: str, source: str | None = None, lineno: int | None = None) -> None:
            msg = repr(option) + " in section " + repr(section) + " already exists"
            if source is not None:
                where = "While reading from " + repr(source)
                if lineno is not None:
                    where += " [line " + f"{lineno:2d}" + "]"
                msg = where + ": option " + msg
            else:
                msg = "Option " + msg
            Error.__init__(self, msg)
            self.section = section
            self.option = option
            self.source = source
            self.lineno = lineno

    class NoOptionError(Error):
        """A requested option was not found."""

        def __init__(self, option: str, section: str) -> None:
            Error.__init__(self, "No option " + repr(option) + " in section: " + repr(section))
            self.option = option
            self.section = section

    class InterpolationError(Error):
        """Base class for interpolation-related exceptions."""

        def __init__(self, option: str, section: str, msg: str) -> None:
            Error.__init__(self, msg)
            self.option = option
            self.section = section

    class InterpolationMissingOptionError(InterpolationError):
        """A string substitution required a setting which was not available."""

        def __init__(self, option: str, section: str, rawval: str, reference: str) -> None:
            msg = ("Bad value substitution: option " + repr(option) + " in section " + repr(section) +
                   " contains an interpolation key " + repr(reference) + " which is not a valid option name. Raw value: " +
                   repr(rawval))
            InterpolationError.__init__(self, option, section, msg)
            self.reference = reference

    class InterpolationSyntaxError(InterpolationError):
        """Raised when the source text contains invalid syntax."""

    class InterpolationDepthError(InterpolationError):
        """Raised when substitutions are nested too deeply."""

        def __init__(self, option: str, section: str, rawval: str) -> None:
            msg = ("Recursion limit exceeded in value substitution: option " + repr(option) + " in section " +
                   repr(section) + " contains an interpolation key which cannot be substituted in " +
                   str(MAX_INTERPOLATION_DEPTH) + " steps. Raw value: " + repr(rawval))
            InterpolationError.__init__(self, option, section, msg)

    class ParsingError(Error):
        """Raised when a configuration file does not follow legal syntax."""

        def __init__(self, source: str, lineno: int = 0, line: str = "") -> None:
            Error.__init__(self, 'Source contains parsing errors: ' + repr(source))
            self.source = source
            self.errors: list[tuple[int, str]] = []
            if lineno:
                self.append(lineno, line)

        def append(self, lineno: int, line: str) -> None:
            self.errors.append((lineno, line))
            self.message += "\n\t[line " + f"{lineno:2d}" + "]: " + repr(line)

        def __str__(self) -> str:
            return self.message

    class MissingSectionHeaderError(ParsingError):
        """Raised when a key-value pair is found before any section header."""

        def __init__(self, filename: str, lineno: int, line: str) -> None:
            Error.__init__(self, "File contains no section headers.\nfile: " + repr(filename) + ", line: " +
                           str(lineno) + "\n" + repr(line))
            self.source = filename
            self.lineno = lineno
            self.line = line
            self.errors = []

        def __str__(self) -> str:
            return self.message

    class Interpolation:
        """Dummy interpolation that passes the value through with no changes."""

        def before_get(self, parser: "RawConfigParser", section: str, option: str, value: str, defaults: dict[str, str | None]) -> str:
            return value

        def before_set(self, parser: "RawConfigParser", section: str, option: str, value: str) -> str:
            return value

        def before_read(self, parser: "RawConfigParser", section: str, option: str, value: str) -> str:
            return value

        def before_write(self, parser: "RawConfigParser", section: str, option: str, value: str) -> str:
            return value

    def _key_end(rest: str, open_: str, close: str) -> int:
        """The end of a %(name)s / ${name} reference at the start of rest (-1: not one)."""
        if not rest.startswith(open_):
            return -1
        j = rest.find(close, len(open_))
        if j <= len(open_):
            return -1
        return j + len(close)

    class BasicInterpolation(Interpolation):
        """%(name)s refers to the option name in the same section (or DEFAULT); %% is a %."""

        def before_get(self, parser: "RawConfigParser", section: str, option: str, value: str, defaults: dict[str, str | None]) -> str:
            accum: list[str] = []
            self._interpolate_some(parser, option, accum, value, section, defaults, 1)
            return ''.join(accum)

        def before_set(self, parser: "RawConfigParser", section: str, option: str, value: str) -> str:
            tmp = value.replace('%%', '')
            out = ""
            i = 0
            while i < len(tmp):
                e = _key_end(tmp[i:], "%(", ")s")
                if e > 0:
                    i += e
                    continue
                out += tmp[i]
                i += 1
            if '%' in out:
                raise ValueError("invalid interpolation syntax in " + repr(value) + " at position " + str(out.find('%')))
            return value

        def _interpolate_some(self, parser: "RawConfigParser", option: str, accum: list[str], rest: str, section: str,
                              map: dict[str, str | None], depth: int) -> None:
            rawval = parser._get_raw(section, option, rest)
            if depth > MAX_INTERPOLATION_DEPTH:
                raise InterpolationDepthError(option, section, rawval)
            while rest:
                p = rest.find("%")
                if p < 0:
                    accum.append(rest)
                    return
                if p > 0:
                    accum.append(rest[:p])
                    rest = rest[p:]
                c = rest[1:2]
                if c == "%":
                    accum.append("%")
                    rest = rest[2:]
                elif c == "(":
                    e = _key_end(rest, "%(", ")s")
                    if e < 0:
                        raise InterpolationSyntaxError(option, section, "bad interpolation variable reference " + repr(rest))
                    var = parser.optionxform(rest[2:e - 2])
                    rest = rest[e:]
                    if var not in map:
                        raise InterpolationMissingOptionError(option, section, rawval, var)
                    v = map[var]
                    if v is None:
                        v = "None"
                    if "%" in v:
                        self._interpolate_some(parser, option, accum, v, section, map, depth + 1)
                    else:
                        accum.append(v)
                else:
                    raise InterpolationSyntaxError(option, section, "'%' must be followed by '%' or '(', found: " + repr(rest))

    class ExtendedInterpolation(Interpolation):
        """${name} (this section or DEFAULT) and ${section:name}; $$ is a $."""

        def before_get(self, parser: "RawConfigParser", section: str, option: str, value: str, defaults: dict[str, str | None]) -> str:
            accum: list[str] = []
            self._interpolate_some(parser, option, accum, value, section, defaults, 1)
            return ''.join(accum)

        def before_set(self, parser: "RawConfigParser", section: str, option: str, value: str) -> str:
            tmp = value.replace('$$', '')
            out = ""
            i = 0
            while i < len(tmp):
                e = _key_end(tmp[i:], "${", "}")
                if e > 0:
                    i += e
                    continue
                out += tmp[i]
                i += 1
            if '$' in out:
                raise ValueError("invalid interpolation syntax in " + repr(value) + " at position " + str(out.find('$')))
            return value

        def _interpolate_some(self, parser: "RawConfigParser", option: str, accum: list[str], rest: str, section: str,
                              map: dict[str, str | None], depth: int) -> None:
            rawval = parser._get_raw(section, option, rest)
            if depth > MAX_INTERPOLATION_DEPTH:
                raise InterpolationDepthError(option, section, rawval)
            while rest:
                p = rest.find("$")
                if p < 0:
                    accum.append(rest)
                    return
                if p > 0:
                    accum.append(rest[:p])
                    rest = rest[p:]
                c = rest[1:2]
                if c == "$":
                    accum.append("$")
                    rest = rest[2:]
                elif c == "{":
                    e = _key_end(rest, "${", "}")
                    if e < 0:
                        raise InterpolationSyntaxError(option, section, "bad interpolation variable reference " + repr(rest))
                    path = rest[2:e - 1].split(':')
                    rest = rest[e:]
                    sect = section
                    opt = option
                    v: str | None = None
                    if len(path) == 1:
                        opt = parser.optionxform(path[0])
                        if opt not in map:
                            raise InterpolationMissingOptionError(option, section, rawval, ":".join(path))
                        v = map[opt]
                    elif len(path) == 2:
                        sect = path[0]
                        opt = parser.optionxform(path[1])
                        if not parser._has_raw(sect, opt):
                            raise InterpolationMissingOptionError(option, section, rawval, ":".join(path))
                        v = parser._get_raw(sect, opt, None)
                    else:
                        raise InterpolationSyntaxError(option, section, "More than one ':' found: " + repr(rest))
                    if v is None:
                        v = "None"
                    if "$" in v:
                        self._interpolate_some(parser, opt, accum, v, sect, parser._unify(sect, None), depth + 1)
                    else:
                        accum.append(v)
                else:
                    raise InterpolationSyntaxError(option, section, "'$' must be followed by '$' or '{', found: " + repr(rest))

    def _lower(s: str) -> str:
        return s.lower()

    class SectionProxy:
        """A proxy for a single section from a parser: config["section"]["key"]."""

        def __init__(self, parser: "RawConfigParser", name: str) -> None:
            self._parser = parser
            self._name = name

        def __repr__(self) -> str:
            return "<Section: " + self._name + ">"

        @property
        def parser(self) -> "RawConfigParser":
            return self._parser

        @property
        def name(self) -> str:
            return self._name

        def __getitem__(self, key: str) -> str:
            if not self._parser.has_option(self._name, key):
                raise KeyError(key)
            return self._parser.get(self._name, key)

        def __setitem__(self, key: str, value: str) -> None:
            self._parser._validate_value(value)
            self._parser.set(self._name, key, value)

        def __delitem__(self, key: str) -> None:
            if not (self._parser.has_option(self._name, key) and self._parser.remove_option(self._name, key)):
                raise KeyError(key)

        def __contains__(self, key: str) -> bool:
            return self._parser.has_option(self._name, key)

        def __len__(self) -> int:
            return len(self._options())

        def __iter__(self):
            for k in self._options():
                yield k

        def keys(self) -> list[str]:
            return self._options()

        def values(self) -> list[str]:
            return [self[k] for k in self._options()]

        def items(self) -> list[tuple[str, str]]:
            return [(k, self[k]) for k in self._options()]

        def _options(self) -> list[str]:
            if self._name != self._parser.default_section:
                return self._parser.options(self._name)
            return list(self._parser.defaults().keys())

        def get(self, option: str, fallback: _F = None, *, raw: bool = False, vars: dict[str, str] | None = None) -> str | None:
            if not self._parser._has_unified(self._name, option, vars):
                if fallback is None:
                    return None
                return str(fallback)
            return self._parser.get(self._name, option, raw=raw, vars=vars)

        def getint(self, option: str, fallback: int | None = None, *, raw: bool = False, vars: dict[str, str] | None = None) -> int | None:
            if not self._parser._has_unified(self._name, option, vars):
                return fallback
            return self._parser.getint(self._name, option, raw=raw, vars=vars)

        def getfloat(self, option: str, fallback: float | None = None, *, raw: bool = False, vars: dict[str, str] | None = None) -> float | None:
            if not self._parser._has_unified(self._name, option, vars):
                return fallback
            return self._parser.getfloat(self._name, option, raw=raw, vars=vars)

        def getboolean(self, option: str, fallback: bool | None = None, *, raw: bool = False, vars: dict[str, str] | None = None) -> bool | None:
            if not self._parser._has_unified(self._name, option, vars):
                return fallback
            return self._parser.getboolean(self._name, option, raw=raw, vars=vars)

    class RawConfigParser:
        """ConfigParser that does not do interpolation."""

        def __init__(self, defaults: dict[str, str] | None = None, dict_type: _D = None, allow_no_value: bool = False, *,
                     delimiters: tuple[str, ...] = ('=', ':'), comment_prefixes: tuple[str, ...] = ('#', ';'),
                     inline_comment_prefixes: tuple[str, ...] | None = None, strict: bool = True,
                     empty_lines_in_values: bool = True, default_section: str = DEFAULTSECT,
                     interpolation: Interpolation | None = None, converters: _V = None,
                     allow_unnamed_section: bool = False) -> None:
            self._sections: dict[str, dict[str, str | None]] = {}
            self._defaults: dict[str, str | None] = {}
            self._proxies: dict[str, SectionProxy] = {}
            self._delimiters = list(delimiters)
            self._comment_prefixes = list(comment_prefixes)
            self._inline_comment_prefixes: list[str] = list(inline_comment_prefixes) if inline_comment_prefixes is not None else []
            self._strict = strict
            self._allow_no_value = allow_no_value
            self._empty_lines_in_values = empty_lines_in_values
            self.default_section = default_section
            self._interpolation: Interpolation = interpolation if interpolation is not None else self._default_interpolation()
            self.optionxform: Callable[[str], str] = _lower
            self._proxies[default_section] = SectionProxy(self, default_section)
            if defaults is not None:
                for key in defaults:
                    self._defaults[self.optionxform(key)] = defaults[key]

        def _default_interpolation(self) -> Interpolation:
            return Interpolation()

        def defaults(self) -> dict[str, str | None]:
            return self._defaults

        def sections(self) -> list[str]:
            """A list of section names, excluding [DEFAULT]."""
            return list(self._sections.keys())

        def add_section(self, section: str) -> None:
            """Creates a new section (ValueError for the default one; DuplicateSectionError if it exists)."""
            if section == self.default_section:
                raise ValueError('Invalid section name: ' + repr(section))
            if section in self._sections:
                raise DuplicateSectionError(section)
            self._sections[section] = {}
            self._proxies[section] = SectionProxy(self, section)

        def has_section(self, section: str) -> bool:
            return section in self._sections

        def options(self, section: str) -> list[str]:
            """A list of option names for the given section name (its own, then the defaults)."""
            if section not in self._sections:
                raise NoSectionError(section)
            out = list(self._sections[section].keys())
            for k in self._defaults:
                if k not in out:
                    out.append(k)
            return out

        def read(self, filenames: _F, encoding: str | None = None) -> list[str]:
            """Reads the files that can be read (a name, or a list of them): the names read."""
            names: list[str] = [filenames] if isinstance(filenames, str) else list(filenames)
            read_ok: list[str] = []
            for filename in names:
                try:
                    with open(filename, encoding=encoding if encoding is not None else "utf-8") as fp:
                        self._read(fp.read().splitlines(True), filename)
                except OSError:
                    continue
                read_ok.append(filename)
            return read_ok

        def read_file(self, f: _F, source: str | None = None) -> None:
            """Reads from a file (or any object with readlines())."""
            name = source if source is not None else "<???>"
            if source is None and hasattr(f, "name"):
                name = str(f.name)
            self._read(f.read().splitlines(True), name)

        def read_string(self, string: str, source: str = '<string>') -> None:
            """Reads the configuration from a string."""
            self._read(string.splitlines(True), source)

        def read_dict(self, dictionary: _D, source: str = '<dict>') -> None:
            """Reads {section: {option: value}} (values made str)."""
            added: set[str] = set()
            for section in dictionary:
                keys = dictionary[section]
                sname = str(section)
                if sname != self.default_section and sname not in self._sections:
                    self.add_section(sname)
                elif self._strict and sname in added:
                    raise DuplicateSectionError(sname, source)
                added.add(sname)
                for key in keys:
                    k = self.optionxform(str(key))
                    if self._strict and sname + "\0" + k in added:
                        raise DuplicateOptionError(sname, k, source)
                    added.add(sname + "\0" + k)
                    self.set(sname, k, str(keys[key]))

        def _unify(self, section: str, vars: dict[str, str] | None) -> dict[str, str | None]:
            d: dict[str, str | None] = dict(self._defaults)
            if section in self._sections:
                for k in self._sections[section]:
                    d[k] = self._sections[section][k]
            elif section != self.default_section:
                raise NoSectionError(section)
            if vars is not None:
                for k in vars:
                    d[self.optionxform(k)] = vars[k]
            return d

        def _has_unified(self, section: str, option: str, vars: dict[str, str] | None) -> bool:
            if section not in self._sections and section != self.default_section:
                return False
            return self.optionxform(option) in self._unify(section, vars)

        def _has_raw(self, section: str, option: str) -> bool:
            return self._has_unified(section, option, None)

        def _get_raw(self, section: str, option: str, fallback: str | None) -> str:
            if self._has_unified(section, option, None):
                v = self._unify(section, None)[self.optionxform(option)]
                return v if v is not None else "None"
            return fallback if fallback is not None else ""

        def get(self, section: str, option: str, *, raw: bool = False, vars: dict[str, str] | None = None,
                fallback: _F = None) -> str:
            """The value of option in section (DEFAULT's when it has none), interpolated unless raw."""
            if section not in self._sections and section != self.default_section:
                if fallback is not None:
                    return str(fallback)
                raise NoSectionError(section)
            d = self._unify(section, vars)
            opt = self.optionxform(option)
            if opt not in d:
                if fallback is not None:
                    return str(fallback)
                raise NoOptionError(opt, section)
            value = d[opt]
            if value is None:
                return "None"
            if raw:
                return value
            return self._interpolation.before_get(self, section, opt, value, d)

        def getint(self, section: str, option: str, *, raw: bool = False, vars: dict[str, str] | None = None,
                   fallback: int | None = None) -> int:
            if fallback is not None and not self._has_unified(section, option, vars):
                return fallback
            return int(self.get(section, option, raw=raw, vars=vars))

        def getfloat(self, section: str, option: str, *, raw: bool = False, vars: dict[str, str] | None = None,
                     fallback: float | None = None) -> float:
            if fallback is not None and not self._has_unified(section, option, vars):
                return fallback
            return float(self.get(section, option, raw=raw, vars=vars))

        def getboolean(self, section: str, option: str, *, raw: bool = False, vars: dict[str, str] | None = None,
                       fallback: bool | None = None) -> bool:
            if fallback is not None and not self._has_unified(section, option, vars):
                return fallback
            return self._convert_to_boolean(self.get(section, option, raw=raw, vars=vars))

        def _convert_to_boolean(self, value: str) -> bool:
            v = value.lower()
            if v not in BOOLEAN_STATES:
                raise ValueError('Not a boolean: ' + value)
            return BOOLEAN_STATES[v]

        def items(self, section: str, raw: bool = False, vars: dict[str, str] | None = None) -> list[tuple[str, str]]:
            """(name, value) pairs of the options in the section."""
            d = self._unify(section, vars)
            out: list[tuple[str, str]] = []
            for option in d:
                v = d[option]
                if v is None:
                    out.append((option, "None"))
                elif raw:
                    out.append((option, v))
                else:
                    out.append((option, self._interpolation.before_get(self, section, option, v, d)))
            return out

        def sections_items(self) -> list[tuple[str, SectionProxy]]:
            return [(name, self._proxies[name]) for name in [self.default_section] + self.sections()]

        def popitem(self) -> tuple[str, SectionProxy]:
            for key in self.sections():
                value = self[key]
                del self._sections[key]
                del self._proxies[key]
                return key, value
            raise KeyError("popitem(): dictionary is empty")

        def has_option(self, section: str, option: str) -> bool:
            """Whether section has option (or DEFAULT has it)."""
            option = self.optionxform(option)
            if not section or section == self.default_section:
                return option in self._defaults
            if section not in self._sections:
                return False
            return option in self._sections[section] or option in self._defaults

        def _validate_value(self, value: str) -> None:
            pass

        def set(self, section: str, option: str, value: str | None = None) -> None:
            """Sets an option."""
            if value is not None:
                value = self._interpolation.before_set(self, section, option, value)
            if not section or section == self.default_section:
                self._defaults[self.optionxform(option)] = value
                return
            if section not in self._sections:
                raise NoSectionError(section)
            self._sections[section][self.optionxform(option)] = value

        def write(self, fp: _S, space_around_delimiters: bool = True) -> None:
            """Writes an .ini-format representation of the configuration to fp."""
            d = " " + self._delimiters[0] + " " if space_around_delimiters else self._delimiters[0]
            if self._defaults:
                self._write_section(fp, self.default_section, self._defaults, d)
            for section in self._sections:
                self._write_section(fp, section, self._sections[section], d)

        def _write_section(self, fp: _S, section_name: str, items: dict[str, str | None], delimiter: str) -> None:
            fp.write("[" + section_name + "]\n")
            for key in items:
                value = items[key]
                if value is not None or not self._allow_no_value:
                    text = delimiter + str(value).replace('\n', '\n\t')
                else:
                    text = ""
                fp.write(key + text + "\n")
            fp.write("\n")

        def remove_option(self, section: str, option: str) -> bool:
            """Removes an option: whether it was there."""
            if not section or section == self.default_section:
                sectdict = self._defaults
            else:
                if section not in self._sections:
                    raise NoSectionError(section)
                sectdict = self._sections[section]
            option = self.optionxform(option)
            existed = option in sectdict
            if existed:
                del sectdict[option]
            return existed

        def remove_section(self, section: str) -> bool:
            """Removes a file section: whether it was there."""
            existed = section in self._sections
            if existed:
                del self._sections[section]
                del self._proxies[section]
            return existed

        def __getitem__(self, key: str) -> SectionProxy:
            if key != self.default_section and not self.has_section(key):
                raise KeyError(key)
            return self._proxies[key]

        def __setitem__(self, key: str, value: _V) -> None:
            if key == self.default_section:
                self._defaults.clear()
            elif key in self._sections:
                self._sections[key].clear()
            if key != self.default_section and key not in self._sections:
                self.add_section(key)
            for k in value:
                self.set(key, str(k), str(value[k]))

        def __delitem__(self, key: str) -> None:
            if key == self.default_section:
                raise ValueError("Cannot remove the default section.")
            if not self.has_section(key):
                raise KeyError(key)
            self.remove_section(key)

        def __contains__(self, key: str) -> bool:
            return key == self.default_section or self.has_section(key)

        def __len__(self) -> int:
            return len(self._sections) + 1

        def __iter__(self):
            yield self.default_section
            for s in self._sections:
                yield s

        def _strip_comment(self, line: str) -> tuple[str, bool]:
            """(the line without its comments, stripped; whether it had any)."""
            trimmed = line.strip()
            for p in self._comment_prefixes:
                if trimmed.startswith(p):
                    return "", True
            clean = trimmed
            for p in self._inline_comment_prefixes:
                i = 0
                while True:
                    k = clean.find(p, i)
                    if k < 0:
                        break
                    if k == 0 or clean[k - 1] in " \t":
                        clean = clean[:k].rstrip()
                        break
                    i = k + 1
            return clean, clean != trimmed

        def _read(self, lines: list[str], fpname: str) -> None:
            cursect: dict[str, str | None] | None = None
            cursect_is_default = False
            sectname = ""
            optname: str | None = None
            indent_level = 0
            parts: dict[str, list[str]] = {}         # (the lines of the values of the current section)
            added: set[str] = set()
            errors: ParsingError | None = None
            lineno = 0
            try:
                for raw_line in lines:
                    lineno += 1
                    clean, has_comments = self._strip_comment(raw_line)
                    if not clean:
                        if self._empty_lines_in_values:
                            if not has_comments and cursect is not None and optname is not None and optname in parts:
                                parts[optname].append('')
                        else:
                            indent_level = 1 << 60
                        continue
                    cur_indent_level = len(raw_line) - len(raw_line.lstrip())
                    if cursect is not None and optname is not None and cur_indent_level > indent_level:
                        if optname not in parts:
                            raise ParsingError(fpname, lineno, raw_line)
                        parts[optname].append(clean)
                        continue
                    indent_level = cur_indent_level
                    if clean.startswith("[") and clean.endswith("]") and len(clean) > 2:
                        if cursect is not None:
                            self._join(cursect, parts)
                        parts = {}
                        sectname = clean[1:-1]
                        if sectname in self._sections:
                            if self._strict and sectname in added:
                                raise DuplicateSectionError(sectname, fpname, lineno)
                            cursect = self._sections[sectname]
                            cursect_is_default = False
                            added.add(sectname)
                        elif sectname == self.default_section:
                            cursect = self._defaults
                            cursect_is_default = True
                        else:
                            cursect = {}
                            self._sections[sectname] = cursect
                            self._proxies[sectname] = SectionProxy(self, sectname)
                            cursect_is_default = False
                            added.add(sectname)
                        optname = None
                        continue
                    if cursect is None:
                        raise MissingSectionHeaderError(fpname, lineno, raw_line)
                    k = -1
                    for dlm in self._delimiters:
                        j = clean.find(dlm)
                        if j >= 0 and (k < 0 or j < k):
                            k = j
                    if k < 0:
                        if self._allow_no_value:
                            optname = self.optionxform(clean.rstrip())
                            cursect[optname] = None
                            if optname in parts:
                                del parts[optname]
                            continue
                        if errors is None:
                            errors = ParsingError(fpname)
                        errors.append(lineno, raw_line)
                        optname = None
                        continue
                    name = clean[:k].rstrip()
                    if not name:
                        if errors is None:
                            errors = ParsingError(fpname)
                        errors.append(lineno, raw_line)
                    optname = self.optionxform(name)
                    key = sectname + "\0" + optname
                    if self._strict and key in added:
                        raise DuplicateOptionError(sectname, optname, fpname, lineno)
                    added.add(key)
                    parts[optname] = [clean[k + 1:].strip()]
                    cursect[optname] = ""
            finally:
                if cursect is not None:
                    self._join(cursect, parts)
            if errors is not None:
                raise errors

        def _join(self, sect: dict[str, str | None], parts: dict[str, list[str]]) -> None:
            for name in parts:
                sect[name] = '\n'.join(parts[name]).rstrip()

    class ConfigParser(RawConfigParser):
        """ConfigParser implementing interpolation (BasicInterpolation by default)."""

        def _default_interpolation(self) -> Interpolation:
            return BasicInterpolation()

        def _validate_value(self, value: str) -> None:
            pass

        def add_section(self, section: str) -> None:
            RawConfigParser.add_section(self, section)

    class SafeConfigParser(ConfigParser):
        pass
