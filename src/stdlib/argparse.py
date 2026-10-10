# Author: Steven J. Bethard <steven.bethard@gmail.com>.
# New maintainer as of 29 August 2019:  Raymond Hettinger <raymond.hettinger@gmail.com>

"""Command-line parsing library

This module is an optparse-inspired command-line parsing library that:

    - handles both optional and positional arguments
    - produces highly informative usage messages
    - supports parsers that dispatch to sub-parsers

The following is a simple usage example that sums integers from the
command-line and writes the result to a file::

    parser = argparse.ArgumentParser(
        description='sum the integers at the command line')
    parser.add_argument(
        'integers', metavar='int', nargs='+', type=int,
        help='an integer to be summed')
    parser.add_argument(
        '--log',
        help='the file where the sum should be written')
    args = parser.parse_args()
    with (open(args.log, 'w') if args.log is not None
          else contextlib.nullcontext(sys.stdout)) as log:
        log.write('%s' % sum(args.integers))

The module contains the following public classes:

    - ArgumentParser -- The main entry point for command-line parsing. As the
        example above shows, the add_argument() method is used to populate
        the parser with actions for optional and positional arguments. Then
        the parse_args() method is invoked to convert the args at the
        command-line into an object with attributes.

    - ArgumentError -- The exception raised by ArgumentParser objects when
        there are errors with the parser's actions. Errors raised while
        parsing the command-line are caught by ArgumentParser and emitted
        as command-line messages.

    - FileType -- A factory for defining types of files to be created. As the
        example above shows, instances of FileType are typically passed as
        the type= argument of add_argument() calls. Deprecated since
        Python 3.14.

    - Action -- The base class for parser actions. Typically actions are
        selected by passing strings like 'store_true' or 'append_const' to
        the action= argument of add_argument(). However, for greater
        customization of ArgumentParser actions, subclasses of Action may
        be defined and passed as the action= argument.

    - HelpFormatter, RawDescriptionHelpFormatter, RawTextHelpFormatter,
        ArgumentDefaultsHelpFormatter -- Formatter classes which
        may be passed as the formatter_class= argument to the
        ArgumentParser constructor. HelpFormatter is the default,
        RawDescriptionHelpFormatter and RawTextHelpFormatter tell the parser
        not to change the formatting for help text, and
        ArgumentDefaultsHelpFormatter adds information about argument defaults
        to the help.

All other classes in this module are considered implementation details.
(Also note that HelpFormatter and RawDescriptionHelpFormatter are only
considered public as object names -- the API of the formatter objects is
still considered an implementation detail.)
"""
import sys

if not sys._compiled:

    __version__ = '1.1'
    __all__ = [
        'ArgumentParser',
        'ArgumentError',
        'ArgumentTypeError',
        'BooleanOptionalAction',
        'FileType',
        'HelpFormatter',
        'ArgumentDefaultsHelpFormatter',
        'RawDescriptionHelpFormatter',
        'RawTextHelpFormatter',
        'MetavarTypeHelpFormatter',
        'Namespace',
        'Action',
        'ONE_OR_MORE',
        'OPTIONAL',
        'PARSER',
        'REMAINDER',
        'SUPPRESS',
        'ZERO_OR_MORE',
    ]


    import os as _os
    import re as _re
    import sys as _sys

    from gettext import gettext as _, ngettext

    SUPPRESS = '==SUPPRESS=='

    OPTIONAL = '?'
    ZERO_OR_MORE = '*'
    ONE_OR_MORE = '+'
    PARSER = 'A...'
    REMAINDER = '...'
    _UNRECOGNIZED_ARGS_ATTR = '_unrecognized_args'

    # =============================
    # Utility functions and classes
    # =============================

    class _AttributeHolder(object):
        """Abstract base class that provides __repr__.

    The __repr__ method returns a string in the format::
        ClassName(attr=name, attr=name, ...)
    The attributes are determined either by a class-level attribute,
    '_kwarg_names', or by inspecting the instance __dict__.
    """

        def __repr__(self):
            type_name = type(self).__name__
            arg_strings = []
            star_args = {}
            for arg in self._get_args():
                arg_strings.append(repr(arg))
            for name, value in self._get_kwargs():
                if name.isidentifier():
                    arg_strings.append('%s=%r' % (name, value))
                else:
                    star_args[name] = value
            if star_args:
                arg_strings.append('**%s' % repr(star_args))
            return '%s(%s)' % (type_name, ', '.join(arg_strings))

        def _get_kwargs(self):
            return list(self.__dict__.items())

        def _get_args(self):
            return []


    def _copy_items(items):
        if items is None:
            return []
        # The copy module is used only in the 'append' and 'append_const'
        # actions, and it is needed only when the default value isn't a list.
        # Delay its import for speeding up the common case.
        if type(items) is list:
            return items[:]
        import copy
        return copy.copy(items)


    def _identity(value):
        return value


    # ===============
    # Formatting Help
    # ===============


    class HelpFormatter(object):
        """Formatter for generating usage messages and argument help strings.

    Only the name of this class is considered a public API. All the methods
    provided by the class are considered an implementation detail.
    """

        def __init__(
            self,
            prog,
            indent_increment=2,
            max_help_position=24,
            width=None,
            color=True,
        ):
            # default setting for width
            if width is None:
                import shutil
                width = shutil.get_terminal_size().columns
                width -= 2

            self._set_color(color)
            self._prog = prog
            self._indent_increment = indent_increment
            self._max_help_position = min(max_help_position,
                                          max(width - 20, indent_increment * 2))
            self._width = width

            self._current_indent = 0
            self._level = 0
            self._action_max_length = 0

            self._root_section = self._Section(self, None)
            self._current_section = self._root_section

            self._whitespace_matcher = _re.compile(r'\s+', _re.ASCII)
            self._long_break_matcher = _re.compile(r'\n\n\n+')

        def _set_color(self, color):
            from _colorize import can_colorize, decolor, get_theme

            if color and can_colorize():
                self._theme = get_theme(force_color=True).argparse
                self._decolor = decolor
            else:
                self._theme = get_theme(force_no_color=True).argparse
                self._decolor = _identity

        # ===============================
        # Section and indentation methods
        # ===============================

        def _indent(self):
            self._current_indent += self._indent_increment
            self._level += 1

        def _dedent(self):
            self._current_indent -= self._indent_increment
            assert self._current_indent >= 0, 'Indent decreased below 0.'
            self._level -= 1

        class _Section(object):

            def __init__(self, formatter, parent, heading=None):
                self.formatter = formatter
                self.parent = parent
                self.heading = heading
                self.items = []

            def format_help(self):
                # format the indented section
                if self.parent is not None:
                    self.formatter._indent()
                join = self.formatter._join_parts
                item_help = join([func(*args) for func, args in self.items])
                if self.parent is not None:
                    self.formatter._dedent()

                # return nothing if the section was empty
                if not item_help:
                    return ''

                # add the heading if the section was non-empty
                if self.heading is not SUPPRESS and self.heading is not None:
                    current_indent = self.formatter._current_indent
                    heading_text = _('%(heading)s:') % dict(heading=self.heading)
                    t = self.formatter._theme
                    heading = (
                        f'{" " * current_indent}'
                        f'{t.heading}{heading_text}{t.reset}\n'
                    )
                else:
                    heading = ''

                # join the section-initial newline, the heading and the help
                return join(['\n', heading, item_help, '\n'])

        def _add_item(self, func, args):
            self._current_section.items.append((func, args))

        # ========================
        # Message building methods
        # ========================

        def start_section(self, heading):
            self._indent()
            section = self._Section(self, self._current_section, heading)
            self._add_item(section.format_help, [])
            self._current_section = section

        def end_section(self):
            self._current_section = self._current_section.parent
            self._dedent()

        def add_text(self, text):
            if text is not SUPPRESS and text is not None:
                self._add_item(self._format_text, [text])

        def add_usage(self, usage, actions, groups, prefix=None):
            if usage is not SUPPRESS:
                args = usage, actions, groups, prefix
                self._add_item(self._format_usage, args)

        def add_argument(self, action):
            if action.help is not SUPPRESS:

                # find all invocations
                get_invocation = lambda x: self._decolor(self._format_action_invocation(x))
                invocation_lengths = [len(get_invocation(action)) + self._current_indent]
                for subaction in self._iter_indented_subactions(action):
                    invocation_lengths.append(len(get_invocation(subaction)) + self._current_indent)

                # update the maximum item length
                action_length = max(invocation_lengths)
                self._action_max_length = max(self._action_max_length,
                                              action_length)

                # add the item to the list
                self._add_item(self._format_action, [action])

        def add_arguments(self, actions):
            for action in actions:
                self.add_argument(action)

        # =======================
        # Help-formatting methods
        # =======================

        def format_help(self):
            help = self._root_section.format_help()
            if help:
                help = self._long_break_matcher.sub('\n\n', help)
                help = help.strip('\n') + '\n'
            return help

        def _join_parts(self, part_strings):
            return ''.join([part
                            for part in part_strings
                            if part and part is not SUPPRESS])

        def _format_usage(self, usage, actions, groups, prefix):
            t = self._theme

            if prefix is None:
                prefix = _('usage: ')

            # if usage is specified, use that
            if usage is not None:
                usage = (
                    t.prog_extra
                    + usage
                    % {"prog": f"{t.prog}{self._prog}{t.reset}{t.prog_extra}"}
                    + t.reset
                )

            # if no optionals or positionals are available, usage is just prog
            elif usage is None and not actions:
                usage = f"{t.prog}{self._prog}{t.reset}"

            # if optionals and positionals are available, calculate usage
            elif usage is None:
                prog = '%(prog)s' % dict(prog=self._prog)

                parts, pos_start = self._get_actions_usage_parts(actions, groups)
                # build full usage string
                usage = ' '.join(filter(None, [prog, *parts]))

                # wrap the usage parts if it's too long
                text_width = self._width - self._current_indent
                if len(prefix) + len(self._decolor(usage)) > text_width:

                    # break usage into wrappable parts
                    opt_parts = parts[:pos_start]
                    pos_parts = parts[pos_start:]

                    # helper for wrapping lines
                    def get_lines(parts, indent, prefix=None):
                        lines = []
                        line = []
                        indent_length = len(indent)
                        if prefix is not None:
                            line_len = len(prefix) - 1
                        else:
                            line_len = indent_length - 1
                        for part in parts:
                            part_len = len(self._decolor(part))
                            if line_len + 1 + part_len > text_width and line:
                                lines.append(indent + ' '.join(line))
                                line = []
                                line_len = indent_length - 1
                            line.append(part)
                            line_len += part_len + 1
                        if line:
                            lines.append(indent + ' '.join(line))
                        if prefix is not None:
                            lines[0] = lines[0][indent_length:]
                        return lines

                    # if prog is short, follow it with optionals or positionals
                    prog_len = len(self._decolor(prog))
                    if len(prefix) + prog_len <= 0.75 * text_width:
                        indent = ' ' * (len(prefix) + prog_len + 1)
                        if opt_parts:
                            lines = get_lines([prog] + opt_parts, indent, prefix)
                            lines.extend(get_lines(pos_parts, indent))
                        elif pos_parts:
                            lines = get_lines([prog] + pos_parts, indent, prefix)
                        else:
                            lines = [prog]

                    # if prog is long, put it on its own line
                    else:
                        indent = ' ' * len(prefix)
                        parts = opt_parts + pos_parts
                        lines = get_lines(parts, indent)
                        if len(lines) > 1:
                            lines = []
                            lines.extend(get_lines(opt_parts, indent))
                            lines.extend(get_lines(pos_parts, indent))
                        lines = [prog] + lines

                    # join lines into usage
                    usage = '\n'.join(lines)

                usage = usage.removeprefix(prog)
                usage = f"{t.prog}{prog}{t.reset}{usage}"

            # prefix with 'usage:'
            return f'{t.usage}{prefix}{t.reset}{usage}\n\n'

        def _is_long_option(self, string):
            return len(string) > 2

        def _get_actions_usage_parts(self, actions, groups):
            """Get usage parts with split index for optionals/positionals.

        Returns (parts, pos_start) where pos_start is the index in parts
        where positionals begin.
        This preserves mutually exclusive group formatting across the
        optionals/positionals boundary (gh-75949).
        """
            actions = [action for action in actions if action.help is not SUPPRESS]
            # group actions by mutually exclusive groups
            action_groups = dict.fromkeys(actions)
            for group in groups:
                for action in group._group_actions:
                    if action in action_groups:
                        action_groups[action] = group
            # positional arguments keep their position
            positionals = []
            for action in actions:
                if not action.option_strings:
                    group = action_groups.pop(action)
                    if group:
                        group_actions = [
                            action2 for action2 in group._group_actions
                            if action2.option_strings and
                               action_groups.pop(action2, None)
                        ] + [action]
                        positionals.append((group.required, group_actions))
                    else:
                        positionals.append((None, [action]))
            # the remaining optional arguments are sorted by the position of
            # the first option in the group
            optionals = []
            for action in actions:
                if action.option_strings and action in action_groups:
                    group = action_groups.pop(action)
                    if group:
                        group_actions = [action] + [
                            action2 for action2 in group._group_actions
                            if action2.option_strings and
                               action_groups.pop(action2, None)
                        ]
                        optionals.append((group.required, group_actions))
                    else:
                        optionals.append((None, [action]))

            # collect all actions format strings
            parts = []
            t = self._theme
            pos_start = None
            for i, (required, group) in enumerate(optionals + positionals):
                start = len(parts)
                if i == len(optionals):
                    pos_start = start
                in_group = len(group) > 1
                for action in group:
                    # produce all arg strings
                    if not action.option_strings:
                        default = self._get_default_metavar_for_positional(action)
                        part = self._format_args(action, default)
                        # if it's in a group, strip the outer []
                        if in_group:
                            if part[0] == '[' and part[-1] == ']':
                                part = part[1:-1]
                        part = t.summary_action + part + t.reset

                    # produce the first way to invoke the option in brackets
                    else:
                        option_string = action.option_strings[0]
                        if self._is_long_option(option_string):
                            option_color = t.summary_long_option
                        else:
                            option_color = t.summary_short_option

                        # if the Optional doesn't take a value, format is:
                        #    -s or --long
                        if action.nargs == 0:
                            part = action.format_usage()
                            part = f"{option_color}{part}{t.reset}"

                        # if the Optional takes a value, format is:
                        #    -s ARGS or --long ARGS
                        else:
                            default = self._get_default_metavar_for_optional(action)
                            args_string = self._format_args(action, default)
                            part = (
                                f"{option_color}{option_string} "
                                f"{t.summary_label}{args_string}{t.reset}"
                            )

                        # make it look optional if it's not required or in a group
                        if not (action.required or required or in_group):
                            part = '[%s]' % part

                    # add the action string to the list
                    parts.append(part)

                if in_group:
                    parts[start] = ('(' if required else '[') + parts[start]
                    for i in range(start, len(parts) - 1):
                        parts[i] += ' |'
                    parts[-1] += ')' if required else ']'

            if pos_start is None:
                pos_start = len(parts)
            return parts, pos_start

        def _format_text(self, text):
            if '%(prog)' in text:
                text = text % dict(prog=self._prog)
            text_width = max(self._width - self._current_indent, 11)
            indent = ' ' * self._current_indent
            return self._fill_text(text, text_width, indent) + '\n\n'

        def _format_action(self, action):
            # determine the required width and the entry label
            help_position = min(self._action_max_length + 2,
                                self._max_help_position)
            help_width = max(self._width - help_position, 11)
            action_width = help_position - self._current_indent - 2
            action_header = self._format_action_invocation(action)
            action_header_no_color = self._decolor(action_header)

            # no help; start on same line and add a final newline
            if not action.help:
                tup = self._current_indent, '', action_header
                action_header = '%*s%s\n' % tup

            # short action name; start on the same line and pad two spaces
            elif len(action_header_no_color) <= action_width:
                # calculate widths without color codes
                action_header_color = action_header
                tup = self._current_indent, '', action_width, action_header_no_color
                action_header = '%*s%-*s  ' % tup
                # swap in the colored header
                action_header = action_header.replace(
                    action_header_no_color, action_header_color
                )
                indent_first = 0

            # long action name; start on the next line
            else:
                tup = self._current_indent, '', action_header
                action_header = '%*s%s\n' % tup
                indent_first = help_position

            # collect the pieces of the action help
            parts = [action_header]

            # if there was help for the action, add lines of help text
            if action.help and action.help.strip():
                help_text = self._expand_help(action)
                if help_text:
                    help_lines = self._split_lines(help_text, help_width)
                    parts.append('%*s%s\n' % (indent_first, '', help_lines[0]))
                    for line in help_lines[1:]:
                        parts.append('%*s%s\n' % (help_position, '', line))

            # or add a newline if the description doesn't end with one
            elif not action_header.endswith('\n'):
                parts.append('\n')

            # if there are any sub-actions, add their help as well
            for subaction in self._iter_indented_subactions(action):
                parts.append(self._format_action(subaction))

            # return a single string
            return self._join_parts(parts)

        def _format_action_invocation(self, action):
            t = self._theme

            if not action.option_strings:
                default = self._get_default_metavar_for_positional(action)
                return (
                    t.action
                    + ' '.join(self._metavar_formatter(action, default)(1))
                    + t.reset
                )

            else:

                def color_option_strings(strings):
                    parts = []
                    for s in strings:
                        if self._is_long_option(s):
                            parts.append(f"{t.long_option}{s}{t.reset}")
                        else:
                            parts.append(f"{t.short_option}{s}{t.reset}")
                    return parts

                # if the Optional doesn't take a value, format is:
                #    -s, --long
                if action.nargs == 0:
                    option_strings = color_option_strings(action.option_strings)
                    return ', '.join(option_strings)

                # if the Optional takes a value, format is:
                #    -s, --long ARGS
                else:
                    default = self._get_default_metavar_for_optional(action)
                    option_strings = color_option_strings(action.option_strings)
                    args_string = (
                        f"{t.label}{self._format_args(action, default)}{t.reset}"
                    )
                    return ', '.join(option_strings) + ' ' + args_string

        def _metavar_formatter(self, action, default_metavar):
            if action.metavar is not None:
                result = action.metavar
            elif action.choices is not None:
                result = '{%s}' % ','.join(map(str, action.choices))
            else:
                result = default_metavar

            def format(tuple_size):
                if isinstance(result, tuple):
                    return result
                else:
                    return (result, ) * tuple_size
            return format

        def _format_args(self, action, default_metavar):
            get_metavar = self._metavar_formatter(action, default_metavar)
            if action.nargs is None:
                result = '%s' % get_metavar(1)
            elif action.nargs == OPTIONAL:
                result = '[%s]' % get_metavar(1)
            elif action.nargs == ZERO_OR_MORE:
                metavar = get_metavar(1)
                if len(metavar) == 2:
                    result = '[%s [%s ...]]' % metavar
                else:
                    result = '[%s ...]' % metavar
            elif action.nargs == ONE_OR_MORE:
                result = '%s [%s ...]' % get_metavar(2)
            elif action.nargs == REMAINDER:
                result = '...'
            elif action.nargs == PARSER:
                result = '%s ...' % get_metavar(1)
            elif action.nargs == SUPPRESS:
                result = ''
            else:
                try:
                    formats = ['%s' for _ in range(action.nargs)]
                except TypeError:
                    raise ValueError("invalid nargs value") from None
                result = ' '.join(formats) % get_metavar(action.nargs)
            return result

        def _expand_help(self, action):
            help_string = self._get_help_string(action)
            if '%' not in help_string:
                return help_string
            params = dict(vars(action), prog=self._prog)
            for name in list(params):
                value = params[name]
                if value is SUPPRESS:
                    del params[name]
                elif hasattr(value, '__name__'):
                    params[name] = value.__name__
            if params.get('choices') is not None:
                params['choices'] = ', '.join(map(str, params['choices']))
            return help_string % params

        def _iter_indented_subactions(self, action):
            try:
                get_subactions = action._get_subactions
            except AttributeError:
                pass
            else:
                self._indent()
                yield from get_subactions()
                self._dedent()

        def _split_lines(self, text, width):
            text = self._whitespace_matcher.sub(' ', text).strip()
            # The textwrap module is used only for formatting help.
            # Delay its import for speeding up the common usage of argparse.
            import textwrap
            return textwrap.wrap(text, width)

        def _fill_text(self, text, width, indent):
            text = self._whitespace_matcher.sub(' ', text).strip()
            import textwrap
            return textwrap.fill(text, width,
                                 initial_indent=indent,
                                 subsequent_indent=indent)

        def _get_help_string(self, action):
            return action.help

        def _get_default_metavar_for_optional(self, action):
            return action.dest.upper()

        def _get_default_metavar_for_positional(self, action):
            return action.dest


    class RawDescriptionHelpFormatter(HelpFormatter):
        """Help message formatter which retains any formatting in descriptions.

    Only the name of this class is considered a public API. All the methods
    provided by the class are considered an implementation detail.
    """

        def _fill_text(self, text, width, indent):
            return ''.join(indent + line for line in text.splitlines(keepends=True))


    class RawTextHelpFormatter(RawDescriptionHelpFormatter):
        """Help message formatter which retains formatting of all help text.

    Only the name of this class is considered a public API. All the methods
    provided by the class are considered an implementation detail.
    """

        def _split_lines(self, text, width):
            return text.splitlines()


    class ArgumentDefaultsHelpFormatter(HelpFormatter):
        """Help message formatter which adds default values to argument help.

    Only the name of this class is considered a public API. All the methods
    provided by the class are considered an implementation detail.
    """

        def _get_help_string(self, action):
            help = action.help
            if help is None:
                help = ''

            if (
                '%(default)' not in help
                and action.default is not SUPPRESS
                and not action.required
            ):
                defaulting_nargs = (OPTIONAL, ZERO_OR_MORE)
                if action.option_strings or action.nargs in defaulting_nargs:
                    help += _(' (default: %(default)s)')
            return help



    class MetavarTypeHelpFormatter(HelpFormatter):
        """Help message formatter which uses the argument 'type' as the default
    metavar value (instead of the argument 'dest')

    Only the name of this class is considered a public API. All the methods
    provided by the class are considered an implementation detail.
    """

        def _get_default_metavar_for_optional(self, action):
            return action.type.__name__

        def _get_default_metavar_for_positional(self, action):
            return action.type.__name__


    # =====================
    # Options and Arguments
    # =====================

    def _get_action_name(argument):
        if argument is None:
            return None
        elif argument.option_strings:
            return '/'.join(argument.option_strings)
        elif argument.metavar not in (None, SUPPRESS):
            metavar = argument.metavar
            if not isinstance(metavar, tuple):
                return metavar
            if argument.nargs == ZERO_OR_MORE and len(metavar) == 2:
                return '%s[, %s]' % metavar
            elif argument.nargs == ONE_OR_MORE:
                return '%s[, %s]' % metavar
            else:
                return ', '.join(metavar)
        elif argument.dest not in (None, SUPPRESS):
            return argument.dest
        elif argument.choices:
            return '{%s}' % ','.join(map(str, argument.choices))
        else:
            return None


    class ArgumentError(Exception):
        """An error from creating or using an argument (optional or positional).

    The string value of this exception is the message, augmented with
    information about the argument that caused it.
    """

        def __init__(self, argument, message):
            self.argument_name = _get_action_name(argument)
            self.message = message

        def __str__(self):
            if self.argument_name is None:
                format = '%(message)s'
            else:
                format = _('argument %(argument_name)s: %(message)s')
            return format % dict(message=self.message,
                                 argument_name=self.argument_name)


    class ArgumentTypeError(Exception):
        """An error from trying to convert a command line string to a type."""
        pass


    # ==============
    # Action classes
    # ==============

    class Action(_AttributeHolder):
        """Information about how to convert command line strings to Python objects.

    Action objects are used by an ArgumentParser to represent the information
    needed to parse a single argument from one or more strings from the
    command line. The keyword arguments to the Action constructor are also
    all attributes of Action instances.

    Keyword Arguments:

        - option_strings -- A list of command-line option strings which
            should be associated with this action.

        - dest -- The name of the attribute to hold the created object(s)

        - nargs -- The number of command-line arguments that should be
            consumed. By default, one argument will be consumed and a single
            value will be produced.  Other values include:
                - N (an integer) consumes N arguments (and produces a list)
                - '?' consumes zero or one arguments
                - '*' consumes zero or more arguments (and produces a list)
                - '+' consumes one or more arguments (and produces a list)
            Note that the difference between the default and nargs=1 is that
            with the default, a single value will be produced, while with
            nargs=1, a list containing a single value will be produced.

        - const -- The value to be produced if the option is specified and the
            option uses an action that takes no values.

        - default -- The value to be produced if the option is not specified.

        - type -- A callable that accepts a single string argument, and
            returns the converted value.  The standard Python types str, int,
            float, and complex are useful examples of such callables.  If None,
            str is used.

        - choices -- A container of values that should be allowed. If not None,
            after a command-line argument has been converted to the appropriate
            type, an exception will be raised if it is not a member of this
            collection.

        - required -- True if the action must always be specified at the
            command line. This is only meaningful for optional command-line
            arguments.

        - help -- The help string describing the argument.

        - metavar -- The name to be used for the option's argument with the
            help string. If None, the 'dest' value will be used as the name.
    """

        def __init__(self,
                     option_strings,
                     dest,
                     nargs=None,
                     const=None,
                     default=None,
                     type=None,
                     choices=None,
                     required=False,
                     help=None,
                     metavar=None,
                     deprecated=False):
            self.option_strings = option_strings
            self.dest = dest
            self.nargs = nargs
            self.const = const
            self.default = default
            self.type = type
            self.choices = choices
            self.required = required
            self.help = help
            self.metavar = metavar
            self.deprecated = deprecated

        def _get_kwargs(self):
            names = [
                'option_strings',
                'dest',
                'nargs',
                'const',
                'default',
                'type',
                'choices',
                'required',
                'help',
                'metavar',
                'deprecated',
            ]
            return [(name, getattr(self, name)) for name in names]

        def format_usage(self):
            return self.option_strings[0]

        def __call__(self, parser, namespace, values, option_string=None):
            raise NotImplementedError('.__call__() not defined')


    class BooleanOptionalAction(Action):
        def __init__(self,
                     option_strings,
                     dest,
                     default=None,
                     required=False,
                     help=None,
                     deprecated=False):

            _option_strings = []
            for option_string in option_strings:
                _option_strings.append(option_string)

                if option_string.startswith('--'):
                    if option_string.startswith('--no-'):
                        raise ValueError(f'invalid option name {option_string!r} '
                                         f'for BooleanOptionalAction')
                    option_string = '--no-' + option_string[2:]
                    _option_strings.append(option_string)

            super().__init__(
                option_strings=_option_strings,
                dest=dest,
                nargs=0,
                default=default,
                required=required,
                help=help,
                deprecated=deprecated)


        def __call__(self, parser, namespace, values, option_string=None):
            if option_string in self.option_strings:
                setattr(namespace, self.dest, not option_string.startswith('--no-'))

        def format_usage(self):
            return ' | '.join(self.option_strings)


    class _StoreAction(Action):

        def __init__(self,
                     option_strings,
                     dest,
                     nargs=None,
                     const=None,
                     default=None,
                     type=None,
                     choices=None,
                     required=False,
                     help=None,
                     metavar=None,
                     deprecated=False):
            if nargs == 0:
                raise ValueError('nargs for store actions must be != 0; if you '
                                 'have nothing to store, actions such as store '
                                 'true or store const may be more appropriate')
            if const is not None and nargs != OPTIONAL:
                raise ValueError('nargs must be %r to supply const' % OPTIONAL)
            super(_StoreAction, self).__init__(
                option_strings=option_strings,
                dest=dest,
                nargs=nargs,
                const=const,
                default=default,
                type=type,
                choices=choices,
                required=required,
                help=help,
                metavar=metavar,
                deprecated=deprecated)

        def __call__(self, parser, namespace, values, option_string=None):
            setattr(namespace, self.dest, values)


    class _StoreConstAction(Action):

        def __init__(self,
                     option_strings,
                     dest,
                     const=None,
                     default=None,
                     required=False,
                     help=None,
                     metavar=None,
                     deprecated=False):
            super(_StoreConstAction, self).__init__(
                option_strings=option_strings,
                dest=dest,
                nargs=0,
                const=const,
                default=default,
                required=required,
                help=help,
                deprecated=deprecated)

        def __call__(self, parser, namespace, values, option_string=None):
            setattr(namespace, self.dest, self.const)


    class _StoreTrueAction(_StoreConstAction):

        def __init__(self,
                     option_strings,
                     dest,
                     default=False,
                     required=False,
                     help=None,
                     deprecated=False):
            super(_StoreTrueAction, self).__init__(
                option_strings=option_strings,
                dest=dest,
                const=True,
                deprecated=deprecated,
                required=required,
                help=help,
                default=default)


    class _StoreFalseAction(_StoreConstAction):

        def __init__(self,
                     option_strings,
                     dest,
                     default=True,
                     required=False,
                     help=None,
                     deprecated=False):
            super(_StoreFalseAction, self).__init__(
                option_strings=option_strings,
                dest=dest,
                const=False,
                default=default,
                required=required,
                help=help,
                deprecated=deprecated)


    class _AppendAction(Action):

        def __init__(self,
                     option_strings,
                     dest,
                     nargs=None,
                     const=None,
                     default=None,
                     type=None,
                     choices=None,
                     required=False,
                     help=None,
                     metavar=None,
                     deprecated=False):
            if nargs == 0:
                raise ValueError('nargs for append actions must be != 0; if arg '
                                 'strings are not supplying the value to append, '
                                 'the append const action may be more appropriate')
            if const is not None and nargs != OPTIONAL:
                raise ValueError('nargs must be %r to supply const' % OPTIONAL)
            super(_AppendAction, self).__init__(
                option_strings=option_strings,
                dest=dest,
                nargs=nargs,
                const=const,
                default=default,
                type=type,
                choices=choices,
                required=required,
                help=help,
                metavar=metavar,
                deprecated=deprecated)

        def __call__(self, parser, namespace, values, option_string=None):
            items = getattr(namespace, self.dest, None)
            items = _copy_items(items)
            items.append(values)
            setattr(namespace, self.dest, items)


    class _AppendConstAction(Action):

        def __init__(self,
                     option_strings,
                     dest,
                     const=None,
                     default=None,
                     required=False,
                     help=None,
                     metavar=None,
                     deprecated=False):
            super(_AppendConstAction, self).__init__(
                option_strings=option_strings,
                dest=dest,
                nargs=0,
                const=const,
                default=default,
                required=required,
                help=help,
                metavar=metavar,
                deprecated=deprecated)

        def __call__(self, parser, namespace, values, option_string=None):
            items = getattr(namespace, self.dest, None)
            items = _copy_items(items)
            items.append(self.const)
            setattr(namespace, self.dest, items)


    class _CountAction(Action):

        def __init__(self,
                     option_strings,
                     dest,
                     default=None,
                     required=False,
                     help=None,
                     deprecated=False):
            super(_CountAction, self).__init__(
                option_strings=option_strings,
                dest=dest,
                nargs=0,
                default=default,
                required=required,
                help=help,
                deprecated=deprecated)

        def __call__(self, parser, namespace, values, option_string=None):
            count = getattr(namespace, self.dest, None)
            if count is None:
                count = 0
            setattr(namespace, self.dest, count + 1)


    class _HelpAction(Action):

        def __init__(self,
                     option_strings,
                     dest=SUPPRESS,
                     default=SUPPRESS,
                     help=None,
                     deprecated=False):
            super(_HelpAction, self).__init__(
                option_strings=option_strings,
                dest=dest,
                default=default,
                nargs=0,
                help=help,
                deprecated=deprecated)

        def __call__(self, parser, namespace, values, option_string=None):
            parser.print_help()
            parser.exit()


    class _VersionAction(Action):

        def __init__(self,
                     option_strings,
                     version=None,
                     dest=SUPPRESS,
                     default=SUPPRESS,
                     help=None,
                     deprecated=False):
            if help is None:
                help = _("show program's version number and exit")
            super(_VersionAction, self).__init__(
                option_strings=option_strings,
                dest=dest,
                default=default,
                nargs=0,
                help=help)
            self.version = version

        def __call__(self, parser, namespace, values, option_string=None):
            version = self.version
            if version is None:
                version = parser.version
            formatter = parser._get_formatter()
            formatter.add_text(version)
            parser._print_message(formatter.format_help(), _sys.stdout)
            parser.exit()


    class _SubParsersAction(Action):

        class _ChoicesPseudoAction(Action):

            def __init__(self, name, aliases, help):
                metavar = dest = name
                if aliases:
                    metavar += ' (%s)' % ', '.join(aliases)
                sup = super(_SubParsersAction._ChoicesPseudoAction, self)
                sup.__init__(option_strings=[], dest=dest, help=help,
                             metavar=metavar)

        def __init__(self,
                     option_strings,
                     prog,
                     parser_class,
                     dest=SUPPRESS,
                     required=False,
                     help=None,
                     metavar=None):

            self._prog_prefix = prog
            self._parser_class = parser_class
            self._name_parser_map = {}
            self._choices_actions = []
            self._deprecated = set()
            self._color = True

            super(_SubParsersAction, self).__init__(
                option_strings=option_strings,
                dest=dest,
                nargs=PARSER,
                choices=self._name_parser_map,
                required=required,
                help=help,
                metavar=metavar)

        def add_parser(self, name, *, deprecated=False, **kwargs):
            # set prog from the existing prefix
            if kwargs.get('prog') is None:
                kwargs['prog'] = '%s %s' % (self._prog_prefix, name)

            # set color
            if kwargs.get('color') is None:
                kwargs['color'] = self._color

            aliases = kwargs.pop('aliases', ())

            if name in self._name_parser_map:
                raise ValueError(f'conflicting subparser: {name}')
            for alias in aliases:
                if alias in self._name_parser_map:
                    raise ValueError(f'conflicting subparser alias: {alias}')

            # create a pseudo-action to hold the choice help
            if 'help' in kwargs:
                help = kwargs.pop('help')
                choice_action = self._ChoicesPseudoAction(name, aliases, help)
                self._choices_actions.append(choice_action)
            else:
                choice_action = None

            # create the parser and add it to the map
            parser = self._parser_class(**kwargs)
            if choice_action is not None:
                parser._check_help(choice_action)
            self._name_parser_map[name] = parser

            # make parser available under aliases also
            for alias in aliases:
                self._name_parser_map[alias] = parser

            if deprecated:
                self._deprecated.add(name)
                self._deprecated.update(aliases)

            return parser

        def _get_subactions(self):
            return self._choices_actions

        def __call__(self, parser, namespace, values, option_string=None):
            parser_name = values[0]
            arg_strings = values[1:]

            # set the parser name if requested
            if self.dest is not SUPPRESS:
                setattr(namespace, self.dest, parser_name)

            # select the parser
            try:
                subparser = self._name_parser_map[parser_name]
            except KeyError:
                args = {'parser_name': parser_name,
                        'choices': ', '.join(self._name_parser_map)}
                msg = _('unknown parser %(parser_name)r (choices: %(choices)s)') % args
                raise ArgumentError(self, msg)

            if parser_name in self._deprecated:
                parser._warning(_("command '%(parser_name)s' is deprecated") %
                                {'parser_name': parser_name})

            # parse all the remaining options into the namespace
            # store any unrecognized options on the object, so that the top
            # level parser can decide what to do with them

            # In case this subparser defines new defaults, we parse them
            # in a new namespace object and then update the original
            # namespace for the relevant parts.
            subnamespace, arg_strings = subparser.parse_known_args(arg_strings, None)
            for key, value in vars(subnamespace).items():
                setattr(namespace, key, value)

            if arg_strings:
                if not hasattr(namespace, _UNRECOGNIZED_ARGS_ATTR):
                    setattr(namespace, _UNRECOGNIZED_ARGS_ATTR, [])
                getattr(namespace, _UNRECOGNIZED_ARGS_ATTR).extend(arg_strings)

    class _ExtendAction(_AppendAction):
        def __call__(self, parser, namespace, values, option_string=None):
            items = getattr(namespace, self.dest, None)
            items = _copy_items(items)
            items.extend(values)
            setattr(namespace, self.dest, items)

    # ==============
    # Type classes
    # ==============

    class FileType(object):
        """Deprecated factory for creating file object types

    Instances of FileType are typically passed as type= arguments to the
    ArgumentParser add_argument() method.

    Keyword Arguments:
        - mode -- A string indicating how the file is to be opened. Accepts the
            same values as the builtin open() function.
        - bufsize -- The file's desired buffer size. Accepts the same values as
            the builtin open() function.
        - encoding -- The file's encoding. Accepts the same values as the
            builtin open() function.
        - errors -- A string indicating how encoding and decoding errors are to
            be handled. Accepts the same value as the builtin open() function.
    """

        def __init__(self, mode='r', bufsize=-1, encoding=None, errors=None):
            import warnings
            warnings.warn(
                "FileType is deprecated. Simply open files after parsing arguments.",
                category=PendingDeprecationWarning,
                stacklevel=2
            )
            self._mode = mode
            self._bufsize = bufsize
            self._encoding = encoding
            self._errors = errors

        def __call__(self, string):
            # the special argument "-" means sys.std{in,out}
            if string == '-':
                if 'r' in self._mode:
                    return _sys.stdin.buffer if 'b' in self._mode else _sys.stdin
                elif any(c in self._mode for c in 'wax'):
                    return _sys.stdout.buffer if 'b' in self._mode else _sys.stdout
                else:
                    msg = _('argument "-" with mode %r') % self._mode
                    raise ValueError(msg)

            # all other arguments are used as file names
            try:
                return open(string, self._mode, self._bufsize, self._encoding,
                            self._errors)
            except OSError as e:
                args = {'filename': string, 'error': e}
                message = _("can't open '%(filename)s': %(error)s")
                raise ArgumentTypeError(message % args)

        def __repr__(self):
            args = self._mode, self._bufsize
            kwargs = [('encoding', self._encoding), ('errors', self._errors)]
            args_str = ', '.join([repr(arg) for arg in args if arg != -1] +
                                 ['%s=%r' % (kw, arg) for kw, arg in kwargs
                                  if arg is not None])
            return '%s(%s)' % (type(self).__name__, args_str)

    # ===========================
    # Optional and Positional Parsing
    # ===========================

    class Namespace(_AttributeHolder):
        """Simple object for storing attributes.

    Implements equality by attribute names and values, and provides a simple
    string representation.
    """

        def __init__(self, **kwargs):
            for name in kwargs:
                setattr(self, name, kwargs[name])

        def __eq__(self, other):
            if not isinstance(other, Namespace):
                return NotImplemented
            return vars(self) == vars(other)

        def __contains__(self, key):
            return key in self.__dict__


    class _ActionsContainer(object):

        def __init__(self,
                     description,
                     prefix_chars,
                     argument_default,
                     conflict_handler):
            super(_ActionsContainer, self).__init__()

            self.description = description
            self.argument_default = argument_default
            self.prefix_chars = prefix_chars
            self.conflict_handler = conflict_handler

            # set up registries
            self._registries = {}

            # register actions
            self.register('action', None, _StoreAction)
            self.register('action', 'store', _StoreAction)
            self.register('action', 'store_const', _StoreConstAction)
            self.register('action', 'store_true', _StoreTrueAction)
            self.register('action', 'store_false', _StoreFalseAction)
            self.register('action', 'append', _AppendAction)
            self.register('action', 'append_const', _AppendConstAction)
            self.register('action', 'count', _CountAction)
            self.register('action', 'help', _HelpAction)
            self.register('action', 'version', _VersionAction)
            self.register('action', 'parsers', _SubParsersAction)
            self.register('action', 'extend', _ExtendAction)

            # raise an exception if the conflict handler is invalid
            self._get_handler()

            # action storage
            self._actions = []
            self._option_string_actions = {}

            # groups
            self._action_groups = []
            self._mutually_exclusive_groups = []

            # defaults storage
            self._defaults = {}

            # determines whether an "option" looks like a negative number
            self._negative_number_matcher = _re.compile(r'-\.?\d')

            # whether or not there are any optionals that look like negative
            # numbers -- uses a list so it can be shared and edited
            self._has_negative_number_optionals = []

        # ====================
        # Registration methods
        # ====================

        def register(self, registry_name, value, object):
            registry = self._registries.setdefault(registry_name, {})
            registry[value] = object

        def _registry_get(self, registry_name, value, default=None):
            return self._registries[registry_name].get(value, default)

        # ==================================
        # Namespace default accessor methods
        # ==================================

        def set_defaults(self, **kwargs):
            self._defaults.update(kwargs)

            # if these defaults match any existing arguments, replace
            # the previous default on the object with the new one
            for action in self._actions:
                if action.dest in kwargs:
                    action.default = kwargs[action.dest]

        def get_default(self, dest):
            for action in self._actions:
                if action.dest == dest and action.default is not None:
                    return action.default
            return self._defaults.get(dest, None)


        # =======================
        # Adding argument actions
        # =======================

        def add_argument(self, *args, **kwargs):
            """
        add_argument(dest, ..., name=value, ...)
        add_argument(option_string, option_string, ..., name=value, ...)
        """

            # if no positional args are supplied or only one is supplied and
            # it doesn't look like an option string, parse a positional
            # argument
            chars = self.prefix_chars
            if not args or len(args) == 1 and args[0][0] not in chars:
                if args and 'dest' in kwargs:
                    raise TypeError('dest supplied twice for positional argument,'
                                    ' did you mean metavar?')
                kwargs = self._get_positional_kwargs(*args, **kwargs)

            # otherwise, we're adding an optional argument
            else:
                kwargs = self._get_optional_kwargs(*args, **kwargs)

            # if no default was supplied, use the parser-level default
            if 'default' not in kwargs:
                dest = kwargs['dest']
                if dest in self._defaults:
                    kwargs['default'] = self._defaults[dest]
                elif self.argument_default is not None:
                    kwargs['default'] = self.argument_default

            # create the action object, and add it to the parser
            action_name = kwargs.get('action')
            action_class = self._pop_action_class(kwargs)
            if not callable(action_class):
                raise ValueError(f'unknown action {action_class!r}')
            action = action_class(**kwargs)

            # raise an error if action for positional argument does not
            # consume arguments
            if not action.option_strings and action.nargs == 0:
                raise ValueError(f'action {action_name!r} is not valid for positional arguments')

            # raise an error if the action type is not callable
            type_func = self._registry_get('type', action.type, action.type)
            if not callable(type_func):
                raise TypeError(f'{type_func!r} is not callable')

            if type_func is FileType:
                raise TypeError(f'{type_func!r} is a FileType class object, '
                                f'instance of it must be passed')

            # raise an error if the metavar does not match the type
            if hasattr(self, "_get_validation_formatter"):
                formatter = self._get_validation_formatter()
                try:
                    formatter._format_args(action, None)
                except TypeError:
                    raise ValueError("length of metavar tuple does not match nargs")
            self._check_help(action)
            return self._add_action(action)

        def add_argument_group(self, *args, **kwargs):
            group = _ArgumentGroup(self, *args, **kwargs)
            self._action_groups.append(group)
            return group

        def add_mutually_exclusive_group(self, **kwargs):
            group = _MutuallyExclusiveGroup(self, **kwargs)
            self._mutually_exclusive_groups.append(group)
            return group

        def _add_action(self, action):
            # resolve any conflicts
            self._check_conflict(action)

            # add to actions list
            self._actions.append(action)
            action.container = self

            # index the action by any option strings it has
            for option_string in action.option_strings:
                self._option_string_actions[option_string] = action

            # set the flag if any option strings look like negative numbers
            for option_string in action.option_strings:
                if self._negative_number_matcher.match(option_string):
                    if not self._has_negative_number_optionals:
                        self._has_negative_number_optionals.append(True)

            # return the created action
            return action

        def _remove_action(self, action):
            self._actions.remove(action)

        def _add_container_actions(self, container):
            # collect groups by titles
            title_group_map = {}
            for group in self._action_groups:
                if group.title in title_group_map:
                    # This branch could happen if a derived class added
                    # groups with duplicated titles in __init__
                    msg = f'cannot merge actions - two groups are named {group.title!r}'
                    raise ValueError(msg)
                title_group_map[group.title] = group

            # map each action to its group
            group_map = {}
            for group in container._action_groups:

                # if a group with the title exists, use that, otherwise
                # create a new group matching the container's group
                if group.title not in title_group_map:
                    title_group_map[group.title] = self.add_argument_group(
                        title=group.title,
                        description=group.description,
                        conflict_handler=group.conflict_handler)

                # map the actions to their new group
                for action in group._group_actions:
                    group_map[action] = title_group_map[group.title]

            # add container's mutually exclusive groups
            # NOTE: if add_mutually_exclusive_group ever gains title= and
            # description= then this code will need to be expanded as above
            for group in container._mutually_exclusive_groups:
                if group._container is container:
                    cont = self
                else:
                    cont = title_group_map[group._container.title]
                mutex_group = cont.add_mutually_exclusive_group(
                    required=group.required)

                # map the actions to their new mutex group
                for action in group._group_actions:
                    group_map[action] = mutex_group

            # add all actions to this container or their group
            for action in container._actions:
                group_map.get(action, self)._add_action(action)

        def _get_positional_kwargs(self, dest, **kwargs):
            # make sure required is not specified
            if 'required' in kwargs:
                msg = "'required' is an invalid argument for positionals"
                raise TypeError(msg)

            # mark positional arguments as required if at least one is
            # always required
            nargs = kwargs.get('nargs')
            if nargs == 0:
                raise ValueError('nargs for positionals must be != 0')
            if nargs not in [OPTIONAL, ZERO_OR_MORE, REMAINDER, SUPPRESS]:
                kwargs['required'] = True

            # return the keyword arguments with no option strings
            return dict(kwargs, dest=dest, option_strings=[])

        def _get_optional_kwargs(self, *args, **kwargs):
            # determine short and long option strings
            option_strings = []
            long_option_strings = []
            for option_string in args:
                # error on strings that don't start with an appropriate prefix
                if not option_string[0] in self.prefix_chars:
                    raise ValueError(
                        f'invalid option string {option_string!r}: '
                        f'must start with a character {self.prefix_chars!r}')

                # strings starting with two prefix characters are long options
                option_strings.append(option_string)
                if len(option_string) > 1 and option_string[1] in self.prefix_chars:
                    long_option_strings.append(option_string)

            # infer destination, '--foo-bar' -> 'foo_bar' and '-x' -> 'x'
            dest = kwargs.pop('dest', None)
            if dest is None:
                if long_option_strings:
                    dest_option_string = long_option_strings[0]
                else:
                    dest_option_string = option_strings[0]
                dest = dest_option_string.lstrip(self.prefix_chars)
                if not dest:
                    msg = f'dest= is required for options like {option_string!r}'
                    raise TypeError(msg)
                dest = dest.replace('-', '_')

            # return the updated keyword arguments
            return dict(kwargs, dest=dest, option_strings=option_strings)

        def _pop_action_class(self, kwargs, default=None):
            action = kwargs.pop('action', default)
            return self._registry_get('action', action, action)

        def _get_handler(self):
            # determine function from conflict handler string
            handler_func_name = '_handle_conflict_%s' % self.conflict_handler
            try:
                return getattr(self, handler_func_name)
            except AttributeError:
                msg = f'invalid conflict_resolution value: {self.conflict_handler!r}'
                raise ValueError(msg)

        def _check_conflict(self, action):

            # find all options that conflict with this option
            confl_optionals = []
            for option_string in action.option_strings:
                if option_string in self._option_string_actions:
                    confl_optional = self._option_string_actions[option_string]
                    confl_optionals.append((option_string, confl_optional))

            # resolve any conflicts
            if confl_optionals:
                conflict_handler = self._get_handler()
                conflict_handler(action, confl_optionals)

        def _handle_conflict_error(self, action, conflicting_actions):
            message = ngettext('conflicting option string: %s',
                               'conflicting option strings: %s',
                               len(conflicting_actions))
            conflict_string = ', '.join([option_string
                                         for option_string, action
                                         in conflicting_actions])
            raise ArgumentError(action, message % conflict_string)

        def _handle_conflict_resolve(self, action, conflicting_actions):

            # remove all conflicting options
            for option_string, action in conflicting_actions:

                # remove the conflicting option
                action.option_strings.remove(option_string)
                self._option_string_actions.pop(option_string, None)

                # if the option now has no option string, remove it from the
                # container holding it
                if not action.option_strings:
                    action.container._remove_action(action)

        def _check_help(self, action):
            if action.help and hasattr(self, "_get_validation_formatter"):
                formatter = self._get_validation_formatter()
                try:
                    formatter._expand_help(action)
                except (ValueError, TypeError, KeyError) as exc:
                    raise ValueError('badly formed help string') from exc


    class _ArgumentGroup(_ActionsContainer):

        def __init__(self, container, title=None, description=None, **kwargs):
            if 'prefix_chars' in kwargs:
                import warnings
                depr_msg = (
                    "The use of the undocumented 'prefix_chars' parameter in "
                    "ArgumentParser.add_argument_group() is deprecated."
                )
                warnings.warn(depr_msg, DeprecationWarning, stacklevel=3)

            # add any missing keyword arguments by checking the container
            update = kwargs.setdefault
            update('conflict_handler', container.conflict_handler)
            update('prefix_chars', container.prefix_chars)
            update('argument_default', container.argument_default)
            super_init = super(_ArgumentGroup, self).__init__
            super_init(description=description, **kwargs)

            # group attributes
            self.title = title
            self._group_actions = []

            # share most attributes with the container
            self._registries = container._registries
            self._actions = container._actions
            self._option_string_actions = container._option_string_actions
            self._defaults = container._defaults
            self._has_negative_number_optionals = \
                container._has_negative_number_optionals
            self._mutually_exclusive_groups = container._mutually_exclusive_groups

        def _add_action(self, action):
            action = super(_ArgumentGroup, self)._add_action(action)
            self._group_actions.append(action)
            return action

        def _remove_action(self, action):
            super(_ArgumentGroup, self)._remove_action(action)
            self._group_actions.remove(action)

        def add_argument_group(self, *args, **kwargs):
            raise ValueError('argument groups cannot be nested')

    class _MutuallyExclusiveGroup(_ArgumentGroup):

        def __init__(self, container, required=False):
            super(_MutuallyExclusiveGroup, self).__init__(container)
            self.required = required
            self._container = container

        def _add_action(self, action):
            if action.required:
                msg = 'mutually exclusive arguments must be optional'
                raise ValueError(msg)
            action = self._container._add_action(action)
            self._group_actions.append(action)
            return action

        def _remove_action(self, action):
            self._container._remove_action(action)
            self._group_actions.remove(action)

        def add_mutually_exclusive_group(self, **kwargs):
            raise ValueError('mutually exclusive groups cannot be nested')

    def _prog_name(prog=None):
        if prog is not None:
            return prog
        arg0 = _sys.argv[0]
        try:
            modspec = _sys.modules['__main__'].__spec__
        except (KeyError, AttributeError):
            # possibly PYTHONSTARTUP or -X presite or other weird edge case
            # no good answer here, so fall back to the default
            modspec = None
        if modspec is None:
            # simple script
            return _os.path.basename(arg0)
        py = _os.path.basename(_sys.executable)
        if modspec.name != '__main__':
            # imported module or package
            modname = modspec.name.removesuffix('.__main__')
            return f'{py} -m {modname}'
        # directory or ZIP file
        return f'{py} {arg0}'


    class ArgumentParser(_AttributeHolder, _ActionsContainer):
        """Object for parsing command line strings into Python objects.

    Keyword Arguments:
        - prog -- The name of the program (default:
            ``os.path.basename(sys.argv[0])``)
        - usage -- A usage message (default: auto-generated from arguments)
        - description -- A description of what the program does
        - epilog -- Text following the argument descriptions
        - parents -- Parsers whose arguments should be copied into this one
        - formatter_class -- HelpFormatter class for printing help messages
        - prefix_chars -- Characters that prefix optional arguments
        - fromfile_prefix_chars -- Characters that prefix files containing
            additional arguments
        - argument_default -- The default value for all arguments
        - conflict_handler -- String indicating how to handle conflicts
        - add_help -- Add a -h/-help option
        - allow_abbrev -- Allow long options to be abbreviated unambiguously
        - exit_on_error -- Determines whether or not ArgumentParser exits with
            error info when an error occurs
        - suggest_on_error - Enables suggestions for mistyped argument choices
            and subparser names (default: ``False``)
        - color - Allow color output in help messages (default: ``False``)
    """

        def __init__(self,
                     prog=None,
                     usage=None,
                     description=None,
                     epilog=None,
                     parents=[],
                     formatter_class=HelpFormatter,
                     prefix_chars='-',
                     fromfile_prefix_chars=None,
                     argument_default=None,
                     conflict_handler='error',
                     add_help=True,
                     allow_abbrev=True,
                     exit_on_error=True,
                     *,
                     suggest_on_error=False,
                     color=True,
                     ):
            superinit = super(ArgumentParser, self).__init__
            superinit(description=description,
                      prefix_chars=prefix_chars,
                      argument_default=argument_default,
                      conflict_handler=conflict_handler)

            self.prog = _prog_name(prog)
            self.usage = usage
            self.epilog = epilog
            self.formatter_class = formatter_class
            self.fromfile_prefix_chars = fromfile_prefix_chars
            self.add_help = add_help
            self.allow_abbrev = allow_abbrev
            self.exit_on_error = exit_on_error
            self.suggest_on_error = suggest_on_error
            self.color = color

            # Cached formatter for validation (avoids repeated _set_color calls)
            self._cached_formatter = None

            add_group = self.add_argument_group
            self._positionals = add_group(_('positional arguments'))
            self._optionals = add_group(_('options'))
            self._subparsers = None

            # register types
            self.register('type', None, _identity)

            # add help argument if necessary
            # (using explicit default to override global argument_default)
            default_prefix = '-' if '-' in prefix_chars else prefix_chars[0]
            if self.add_help:
                self.add_argument(
                    default_prefix+'h', default_prefix*2+'help',
                    action='help', default=SUPPRESS,
                    help=_('show this help message and exit'))

            # add parent arguments and defaults
            for parent in parents:
                if not isinstance(parent, ArgumentParser):
                    raise TypeError('parents must be a list of ArgumentParser')
                self._add_container_actions(parent)
                defaults = parent._defaults
                self._defaults.update(defaults)

        # =======================
        # Pretty __repr__ methods
        # =======================

        def _get_kwargs(self):
            names = [
                'prog',
                'usage',
                'description',
                'formatter_class',
                'conflict_handler',
                'add_help',
            ]
            return [(name, getattr(self, name)) for name in names]

        # ==================================
        # Optional/Positional adding methods
        # ==================================

        def add_subparsers(self, **kwargs):
            if self._subparsers is not None:
                raise ValueError('cannot have multiple subparser arguments')

            # add the parser class to the arguments if it's not present
            kwargs.setdefault('parser_class', type(self))

            if 'title' in kwargs or 'description' in kwargs:
                title = kwargs.pop('title', _('subcommands'))
                description = kwargs.pop('description', None)
                self._subparsers = self.add_argument_group(title, description)
            else:
                self._subparsers = self._positionals

            # prog defaults to the usage message of this parser, skipping
            # optional arguments and with no "usage:" prefix
            if kwargs.get('prog') is None:
                # Create formatter without color to avoid storing ANSI codes in prog
                formatter = self.formatter_class(prog=self.prog)
                formatter._set_color(False)
                positionals = self._get_positional_actions()
                groups = self._mutually_exclusive_groups
                formatter.add_usage(None, positionals, groups, '')
                kwargs['prog'] = formatter.format_help().strip()

            # create the parsers action and add it to the positionals list
            parsers_class = self._pop_action_class(kwargs, 'parsers')
            action = parsers_class(option_strings=[], **kwargs)
            action._color = self.color
            self._check_help(action)
            self._subparsers._add_action(action)

            # return the created parsers action
            return action

        def _add_action(self, action):
            if action.option_strings:
                self._optionals._add_action(action)
            else:
                self._positionals._add_action(action)
            return action

        def _get_optional_actions(self):
            return [action
                    for action in self._actions
                    if action.option_strings]

        def _get_positional_actions(self):
            return [action
                    for action in self._actions
                    if not action.option_strings]

        # =====================================
        # Command line argument parsing methods
        # =====================================

        def parse_args(self, args=None, namespace=None):
            args, argv = self.parse_known_args(args, namespace)
            if argv:
                msg = _('unrecognized arguments: %s') % ' '.join(argv)
                if self.exit_on_error:
                    self.error(msg)
                else:
                    raise ArgumentError(None, msg)
            return args

        def parse_known_args(self, args=None, namespace=None):
            return self._parse_known_args2(args, namespace, intermixed=False)

        def _parse_known_args2(self, args, namespace, intermixed):
            if args is None:
                # args default to the system args
                args = _sys.argv[1:]
            else:
                # make sure that args are mutable
                args = list(args)

            # default Namespace built from parser defaults
            if namespace is None:
                namespace = Namespace()

            # add any action defaults that aren't present
            for action in self._actions:
                if action.dest is not SUPPRESS:
                    if not hasattr(namespace, action.dest):
                        if action.default is not SUPPRESS:
                            setattr(namespace, action.dest, action.default)

            # add any parser defaults that aren't present
            for dest in self._defaults:
                if not hasattr(namespace, dest):
                    setattr(namespace, dest, self._defaults[dest])

            # parse the arguments and exit if there are any errors
            if self.exit_on_error:
                try:
                    namespace, args = self._parse_known_args(args, namespace, intermixed)
                except ArgumentError as err:
                    self.error(str(err))
            else:
                namespace, args = self._parse_known_args(args, namespace, intermixed)

            if hasattr(namespace, _UNRECOGNIZED_ARGS_ATTR):
                args.extend(getattr(namespace, _UNRECOGNIZED_ARGS_ATTR))
                delattr(namespace, _UNRECOGNIZED_ARGS_ATTR)
            return namespace, args

        def _parse_known_args(self, arg_strings, namespace, intermixed):
            # replace arg strings that are file references
            if self.fromfile_prefix_chars is not None:
                arg_strings = self._read_args_from_files(arg_strings)

            # map all mutually exclusive arguments to the other arguments
            # they can't occur with
            action_conflicts = {}
            for mutex_group in self._mutually_exclusive_groups:
                group_actions = mutex_group._group_actions
                for i, mutex_action in enumerate(mutex_group._group_actions):
                    conflicts = action_conflicts.setdefault(mutex_action, [])
                    conflicts.extend(group_actions[:i])
                    conflicts.extend(group_actions[i + 1:])

            # find all option indices, and determine the arg_string_pattern
            # which has an 'O' if there is an option at an index,
            # an 'A' if there is an argument, or a '-' if there is a '--'
            option_string_indices = {}
            arg_string_pattern_parts = []
            arg_strings_iter = iter(arg_strings)
            for i, arg_string in enumerate(arg_strings_iter):

                # all args after -- are non-options
                if arg_string == '--':
                    arg_string_pattern_parts.append('-')
                    for arg_string in arg_strings_iter:
                        arg_string_pattern_parts.append('A')

                # otherwise, add the arg to the arg strings
                # and note the index if it was an option
                else:
                    option_tuples = self._parse_optional(arg_string)
                    if option_tuples is None:
                        pattern = 'A'
                    else:
                        option_string_indices[i] = option_tuples
                        pattern = 'O'
                    arg_string_pattern_parts.append(pattern)

            # join the pieces together to form the pattern
            arg_strings_pattern = ''.join(arg_string_pattern_parts)

            # converts arg strings to the appropriate and then takes the action
            seen_actions = set()
            seen_non_default_actions = set()
            warned = set()

            def take_action(action, argument_strings, option_string=None):
                seen_actions.add(action)
                argument_values = self._get_values(action, argument_strings)

                # error if this argument is not allowed with other previously
                # seen arguments
                if action.option_strings or argument_strings:
                    seen_non_default_actions.add(action)
                    for conflict_action in action_conflicts.get(action, []):
                        if conflict_action in seen_non_default_actions:
                            msg = _('not allowed with argument %s')
                            action_name = _get_action_name(conflict_action)
                            raise ArgumentError(action, msg % action_name)

                # take the action if we didn't receive a SUPPRESS value
                # (e.g. from a default)
                if argument_values is not SUPPRESS:
                    action(self, namespace, argument_values, option_string)

            # function to convert arg_strings into an optional action
            def consume_optional(start_index):

                # get the optional identified at this index
                option_tuples = option_string_indices[start_index]
                # if multiple actions match, the option string was ambiguous
                if len(option_tuples) > 1:
                    options = ', '.join([option_string
                        for action, option_string, sep, explicit_arg in option_tuples])
                    args = {'option': arg_strings[start_index], 'matches': options}
                    msg = _('ambiguous option: %(option)s could match %(matches)s')
                    raise ArgumentError(None, msg % args)

                action, option_string, sep, explicit_arg = option_tuples[0]

                # identify additional optionals in the same arg string
                # (e.g. -xyz is the same as -x -y -z if no args are required)
                match_argument = self._match_argument
                action_tuples = []
                while True:

                    # if we found no optional action, skip it
                    if action is None:
                        extras.append(arg_strings[start_index])
                        extras_pattern.append('O')
                        return start_index + 1

                    # if there is an explicit argument, try to match the
                    # optional's string arguments to only this
                    if explicit_arg is not None:
                        arg_count = match_argument(action, 'A')

                        # if the action is a single-dash option and takes no
                        # arguments, try to parse more single-dash options out
                        # of the tail of the option string
                        chars = self.prefix_chars
                        if (
                            arg_count == 0
                            and option_string[1] not in chars
                            and explicit_arg != ''
                        ):
                            if sep or explicit_arg[0] in chars:
                                msg = _('ignored explicit argument %r')
                                raise ArgumentError(action, msg % explicit_arg)
                            action_tuples.append((action, [], option_string))
                            char = option_string[0]
                            option_string = char + explicit_arg[0]
                            optionals_map = self._option_string_actions
                            if option_string in optionals_map:
                                action = optionals_map[option_string]
                                explicit_arg = explicit_arg[1:]
                                if not explicit_arg:
                                    sep = explicit_arg = None
                                elif explicit_arg[0] == '=':
                                    sep = '='
                                    explicit_arg = explicit_arg[1:]
                                else:
                                    sep = ''
                            else:
                                extras.append(char + explicit_arg)
                                extras_pattern.append('O')
                                stop = start_index + 1
                                break
                        # if the action expect exactly one argument, we've
                        # successfully matched the option; exit the loop
                        elif arg_count == 1:
                            stop = start_index + 1
                            args = [explicit_arg]
                            action_tuples.append((action, args, option_string))
                            break

                        # error if a double-dash option did not use the
                        # explicit argument
                        else:
                            msg = _('ignored explicit argument %r')
                            raise ArgumentError(action, msg % explicit_arg)

                    # if there is no explicit argument, try to match the
                    # optional's string arguments with the following strings
                    # if successful, exit the loop
                    else:
                        start = start_index + 1
                        selected_patterns = arg_strings_pattern[start:]
                        arg_count = match_argument(action, selected_patterns)
                        stop = start + arg_count
                        args = arg_strings[start:stop]
                        action_tuples.append((action, args, option_string))
                        break

                # add the Optional to the list and return the index at which
                # the Optional's string args stopped
                assert action_tuples
                for action, args, option_string in action_tuples:
                    if action.deprecated and option_string not in warned:
                        self._warning(_("option '%(option)s' is deprecated") %
                                      {'option': option_string})
                        warned.add(option_string)
                    take_action(action, args, option_string)
                return stop

            # the list of Positionals left to be parsed; this is modified
            # by consume_positionals()
            positionals = self._get_positional_actions()

            # function to convert arg_strings into positional actions
            def consume_positionals(start_index):
                # match as many Positionals as possible
                match_partial = self._match_arguments_partial
                selected_pattern = arg_strings_pattern[start_index:]
                arg_counts = match_partial(positionals, selected_pattern)

                # slice off the appropriate arg strings for each Positional
                # and add the Positional and its args to the list
                for action, arg_count in zip(positionals, arg_counts):
                    args = arg_strings[start_index: start_index + arg_count]
                    # Strip out the first '--' if it is not in REMAINDER arg.
                    if action.nargs == PARSER:
                        if arg_strings_pattern[start_index] == '-':
                            assert args[0] == '--'
                            args.remove('--')
                    elif action.nargs != REMAINDER:
                        if (arg_strings_pattern.find('-', start_index,
                                                     start_index + arg_count) >= 0):
                            args.remove('--')
                    start_index += arg_count
                    if args and action.deprecated and action.dest not in warned:
                        self._warning(_("argument '%(argument_name)s' is deprecated") %
                                      {'argument_name': action.dest})
                        warned.add(action.dest)
                    take_action(action, args)

                # slice off the Positionals that we just parsed and return the
                # index at which the Positionals' string args stopped
                positionals[:] = positionals[len(arg_counts):]
                return start_index

            # consume Positionals and Optionals alternately, until we have
            # passed the last option string
            extras = []
            extras_pattern = []
            start_index = 0
            if option_string_indices:
                max_option_string_index = max(option_string_indices)
            else:
                max_option_string_index = -1
            while start_index <= max_option_string_index:

                # consume any Positionals preceding the next option
                next_option_string_index = start_index
                while next_option_string_index <= max_option_string_index:
                    if next_option_string_index in option_string_indices:
                        break
                    next_option_string_index += 1
                if not intermixed and start_index != next_option_string_index:
                    positionals_end_index = consume_positionals(start_index)

                    # only try to parse the next optional if we didn't consume
                    # the option string during the positionals parsing
                    if positionals_end_index > start_index:
                        start_index = positionals_end_index
                        continue
                    else:
                        start_index = positionals_end_index

                # if we consumed all the positionals we could and we're not
                # at the index of an option string, there were extra arguments
                if start_index not in option_string_indices:
                    strings = arg_strings[start_index:next_option_string_index]
                    extras.extend(strings)
                    extras_pattern.extend(arg_strings_pattern[start_index:next_option_string_index])
                    start_index = next_option_string_index

                # consume the next optional and any arguments for it
                start_index = consume_optional(start_index)

            if not intermixed:
                # consume any positionals following the last Optional
                stop_index = consume_positionals(start_index)

                # if we didn't consume all the argument strings, there were extras
                extras.extend(arg_strings[stop_index:])
            else:
                extras.extend(arg_strings[start_index:])
                extras_pattern.extend(arg_strings_pattern[start_index:])
                extras_pattern = ''.join(extras_pattern)
                assert len(extras_pattern) == len(extras)
                # consume all positionals
                arg_strings = [s for s, c in zip(extras, extras_pattern) if c != 'O']
                arg_strings_pattern = extras_pattern.replace('O', '')
                stop_index = consume_positionals(0)
                # leave unknown optionals and non-consumed positionals in extras
                for i, c in enumerate(extras_pattern):
                    if not stop_index:
                        break
                    if c != 'O':
                        stop_index -= 1
                        extras[i] = None
                extras = [s for s in extras if s is not None]

            # make sure all required actions were present and also convert
            # action defaults which were not given as arguments
            required_actions = []
            for action in self._actions:
                if action not in seen_actions:
                    if action.required:
                        required_actions.append(_get_action_name(action))
                    else:
                        # Convert action default now instead of doing it before
                        # parsing arguments to avoid calling convert functions
                        # twice (which may fail) if the argument was given, but
                        # only if it was defined already in the namespace
                        if (action.default is not None and
                            isinstance(action.default, str) and
                            hasattr(namespace, action.dest) and
                            action.default is getattr(namespace, action.dest)):
                            setattr(namespace, action.dest,
                                    self._get_value(action, action.default))

            if required_actions:
                raise ArgumentError(None, _('the following arguments are required: %s') %
                           ', '.join(required_actions))

            # make sure all required groups had one option present
            for group in self._mutually_exclusive_groups:
                if group.required:
                    for action in group._group_actions:
                        if action in seen_non_default_actions:
                            break

                    # if no actions were used, report the error
                    else:
                        names = [_get_action_name(action)
                                 for action in group._group_actions
                                 if action.help is not SUPPRESS]
                        msg = _('one of the arguments %s is required')
                        raise ArgumentError(None, msg % ' '.join(names))

            # return the updated namespace and the extra arguments
            return namespace, extras

        def _read_args_from_files(self, arg_strings):
            # expand arguments referencing files
            new_arg_strings = []
            for arg_string in arg_strings:

                # for regular arguments, just add them back into the list
                if not arg_string or arg_string[0] not in self.fromfile_prefix_chars:
                    new_arg_strings.append(arg_string)

                # replace arguments referencing files with the file content
                else:
                    try:
                        with open(arg_string[1:],
                                  encoding=_sys.getfilesystemencoding(),
                                  errors=_sys.getfilesystemencodeerrors()) as args_file:
                            arg_strings = []
                            for arg_line in args_file.read().splitlines():
                                for arg in self.convert_arg_line_to_args(arg_line):
                                    arg_strings.append(arg)
                            arg_strings = self._read_args_from_files(arg_strings)
                            new_arg_strings.extend(arg_strings)
                    except OSError as err:
                        raise ArgumentError(None, str(err))

            # return the modified argument list
            return new_arg_strings

        def convert_arg_line_to_args(self, arg_line):
            return [arg_line]

        def _match_argument(self, action, arg_strings_pattern):
            # match the pattern for this action to the arg strings
            nargs_pattern = self._get_nargs_pattern(action)
            match = _re.match(nargs_pattern, arg_strings_pattern)

            # raise an exception if we weren't able to find a match
            if match is None:
                nargs_errors = {
                    None: _('expected one argument'),
                    OPTIONAL: _('expected at most one argument'),
                    ONE_OR_MORE: _('expected at least one argument'),
                }
                msg = nargs_errors.get(action.nargs)
                if msg is None:
                    msg = ngettext('expected %s argument',
                                   'expected %s arguments',
                                   action.nargs) % action.nargs
                raise ArgumentError(action, msg)

            # return the number of arguments matched
            return len(match.group(1))

        def _match_arguments_partial(self, actions, arg_strings_pattern):
            # progressively shorten the actions list by slicing off the
            # final actions until we find a match
            for i in range(len(actions), 0, -1):
                actions_slice = actions[:i]
                pattern = ''.join([self._get_nargs_pattern(action)
                                   for action in actions_slice])
                match = _re.match(pattern, arg_strings_pattern)
                if match is not None:
                    result = [len(string) for string in match.groups()]
                    if (match.end() < len(arg_strings_pattern)
                        and arg_strings_pattern[match.end()] == 'O'):
                        while result and not result[-1]:
                            del result[-1]
                    return result
            return []

        def _parse_optional(self, arg_string):
            # if it's an empty string, it was meant to be a positional
            if not arg_string:
                return None

            # if it doesn't start with a prefix, it was meant to be positional
            if not arg_string[0] in self.prefix_chars:
                return None

            # if the option string is present in the parser, return the action
            if arg_string in self._option_string_actions:
                action = self._option_string_actions[arg_string]
                return [(action, arg_string, None, None)]

            # if it's just a single character, it was meant to be positional
            if len(arg_string) == 1:
                return None

            # if the option string before the "=" is present, return the action
            option_string, sep, explicit_arg = arg_string.partition('=')
            if sep and option_string in self._option_string_actions:
                action = self._option_string_actions[option_string]
                return [(action, option_string, sep, explicit_arg)]

            # search through all possible prefixes of the option string
            # and all actions in the parser for possible interpretations
            option_tuples = self._get_option_tuples(arg_string)

            if option_tuples:
                return option_tuples

            # if it was not found as an option, but it looks like a negative
            # number, it was meant to be positional
            # unless there are negative-number-like options
            if self._negative_number_matcher.match(arg_string):
                if not self._has_negative_number_optionals:
                    return None

            # if it contains a space, it was meant to be a positional
            if ' ' in arg_string:
                return None

            # it was meant to be an optional but there is no such option
            # in this parser (though it might be a valid option in a subparser)
            return [(None, arg_string, None, None)]

        def _get_option_tuples(self, option_string):
            result = []

            # option strings starting with two prefix characters are only
            # split at the '='
            chars = self.prefix_chars
            if option_string[0] in chars and option_string[1] in chars:
                if self.allow_abbrev:
                    option_prefix, sep, explicit_arg = option_string.partition('=')
                    if not sep:
                        sep = explicit_arg = None
                    for option_string in self._option_string_actions:
                        if option_string.startswith(option_prefix):
                            action = self._option_string_actions[option_string]
                            tup = action, option_string, sep, explicit_arg
                            result.append(tup)

            # single character options can be concatenated with their arguments
            # but multiple character options always have to have their argument
            # separate
            elif option_string[0] in chars and option_string[1] not in chars:
                option_prefix, sep, explicit_arg = option_string.partition('=')
                if not sep:
                    sep = explicit_arg = None
                short_option_prefix = option_string[:2]
                short_explicit_arg = option_string[2:]

                for option_string in self._option_string_actions:
                    if option_string == short_option_prefix:
                        action = self._option_string_actions[option_string]
                        tup = action, option_string, '', short_explicit_arg
                        result.append(tup)
                    elif self.allow_abbrev and option_string.startswith(option_prefix):
                        action = self._option_string_actions[option_string]
                        tup = action, option_string, sep, explicit_arg
                        result.append(tup)

            # shouldn't ever get here
            else:
                raise ArgumentError(None, _('unexpected option string: %s') % option_string)

            # return the collected option tuples
            return result

        def _get_nargs_pattern(self, action):
            # in all examples below, we have to allow for '--' args
            # which are represented as '-' in the pattern
            nargs = action.nargs
            # if this is an optional action, -- is not allowed
            option = action.option_strings

            # the default (None) is assumed to be a single argument
            if nargs is None:
                nargs_pattern = '([A])' if option else '(-*A-*)'

            # allow zero or one arguments
            elif nargs == OPTIONAL:
                nargs_pattern = '(A?)' if option else '(-*A?-*)'

            # allow zero or more arguments
            elif nargs == ZERO_OR_MORE:
                nargs_pattern = '(A*)' if option else '(-*[A-]*)'

            # allow one or more arguments
            elif nargs == ONE_OR_MORE:
                nargs_pattern = '(A+)' if option else '(-*A[A-]*)'

            # allow any number of options or arguments
            elif nargs == REMAINDER:
                nargs_pattern = '([AO]*)' if option else '(.*)'

            # allow one argument followed by any number of options or arguments
            elif nargs == PARSER:
                nargs_pattern = '(A[AO]*)' if option else '(-*A[-AO]*)'

            # suppress action, like nargs=0
            elif nargs == SUPPRESS:
                nargs_pattern = '()' if option else '(-*)'

            # all others should be integers
            else:
                nargs_pattern = '([AO]{%d})' % nargs if option else '((?:-*A){%d}-*)' % nargs

            # return the pattern
            return nargs_pattern

        # ========================
        # Alt command line argument parsing, allowing free intermix
        # ========================

        def parse_intermixed_args(self, args=None, namespace=None):
            args, argv = self.parse_known_intermixed_args(args, namespace)
            if argv:
                msg = _('unrecognized arguments: %s') % ' '.join(argv)
                if self.exit_on_error:
                    self.error(msg)
                else:
                    raise ArgumentError(None, msg)
            return args

        def parse_known_intermixed_args(self, args=None, namespace=None):
            # returns a namespace and list of extras
            #
            # positional can be freely intermixed with optionals.  optionals are
            # first parsed with all positional arguments deactivated.  The 'extras'
            # are then parsed.  If the parser definition is incompatible with the
            # intermixed assumptions (e.g. use of REMAINDER, subparsers) a
            # TypeError is raised.

            positionals = self._get_positional_actions()
            a = [action for action in positionals
                 if action.nargs in [PARSER, REMAINDER]]
            if a:
                raise TypeError('parse_intermixed_args: positional arg'
                                ' with nargs=%s'%a[0].nargs)

            return self._parse_known_args2(args, namespace, intermixed=True)

        # ========================
        # Value conversion methods
        # ========================

        def _get_values(self, action, arg_strings):
            # optional argument produces a default when not present
            if not arg_strings and action.nargs == OPTIONAL:
                if action.option_strings:
                    value = action.const
                else:
                    value = action.default
                if isinstance(value, str) and value is not SUPPRESS:
                    value = self._get_value(action, value)

            # when nargs='*' on a positional, if there were no command-line
            # args, use the default if it is anything other than None
            elif (not arg_strings and action.nargs == ZERO_OR_MORE and
                  not action.option_strings):
                if action.default is not None:
                    value = action.default
                else:
                    value = []

            # single argument or optional argument produces a single value
            elif len(arg_strings) == 1 and action.nargs in [None, OPTIONAL]:
                arg_string, = arg_strings
                value = self._get_value(action, arg_string)
                self._check_value(action, value)

            # REMAINDER arguments convert all values, checking none
            elif action.nargs == REMAINDER:
                value = [self._get_value(action, v) for v in arg_strings]

            # PARSER arguments convert all values, but check only the first
            elif action.nargs == PARSER:
                value = [self._get_value(action, v) for v in arg_strings]
                self._check_value(action, value[0])

            # SUPPRESS argument does not put anything in the namespace
            elif action.nargs == SUPPRESS:
                value = SUPPRESS

            # all other types of nargs produce a list
            else:
                value = [self._get_value(action, v) for v in arg_strings]
                for v in value:
                    self._check_value(action, v)

            # return the converted value
            return value

        def _get_value(self, action, arg_string):
            type_func = self._registry_get('type', action.type, action.type)
            if not callable(type_func):
                raise TypeError(f'{type_func!r} is not callable')

            # convert the value to the appropriate type
            try:
                result = type_func(arg_string)

            # ArgumentTypeErrors indicate errors
            except ArgumentTypeError as err:
                msg = str(err)
                raise ArgumentError(action, msg)

            # TypeErrors or ValueErrors also indicate errors
            except (TypeError, ValueError):
                name = getattr(action.type, '__name__', repr(action.type))
                args = {'type': name, 'value': arg_string}
                msg = _('invalid %(type)s value: %(value)r')
                raise ArgumentError(action, msg % args)

            # return the converted value
            return result

        def _check_value(self, action, value):
            # converted value must be one of the choices (if specified)
            choices = action.choices
            if choices is None:
                return

            if isinstance(choices, str):
                choices = iter(choices)

            if value not in choices:
                args = {'value': str(value),
                        'choices': ', '.join(repr(str(choice)) for choice in action.choices)}
                msg = _('invalid choice: %(value)r (choose from %(choices)s)')

                if self.suggest_on_error and isinstance(value, str):
                    if all(isinstance(choice, str) for choice in action.choices):
                        import difflib
                        suggestions = difflib.get_close_matches(value, action.choices, 1)
                        if suggestions:
                            args['closest'] = suggestions[0]
                            msg = _('invalid choice: %(value)r, maybe you meant %(closest)r? '
                                    '(choose from %(choices)s)')

                raise ArgumentError(action, msg % args)

        # =======================
        # Help-formatting methods
        # =======================

        def format_usage(self):
            formatter = self._get_formatter()
            formatter.add_usage(self.usage, self._actions,
                                self._mutually_exclusive_groups)
            return formatter.format_help()

        def format_help(self):
            formatter = self._get_formatter()

            # usage
            formatter.add_usage(self.usage, self._actions,
                                self._mutually_exclusive_groups)

            # description
            formatter.add_text(self.description)

            # positionals, optionals and user-defined groups
            for action_group in self._action_groups:
                formatter.start_section(action_group.title)
                formatter.add_text(action_group.description)
                formatter.add_arguments(action_group._group_actions)
                formatter.end_section()

            # epilog
            formatter.add_text(self.epilog)

            # determine help from format above
            return formatter.format_help()

        def _get_formatter(self):
            formatter = self.formatter_class(prog=self.prog)
            formatter._set_color(self.color)
            return formatter

        def _get_validation_formatter(self):
            # Return cached formatter for read-only validation operations
            # (_expand_help and _format_args). Avoids repeated slow _set_color calls.
            if self._cached_formatter is None:
                self._cached_formatter = self._get_formatter()
            return self._cached_formatter

        # =====================
        # Help-printing methods
        # =====================

        def print_usage(self, file=None):
            if file is None:
                file = _sys.stdout
            self._print_message(self.format_usage(), file)

        def print_help(self, file=None):
            if file is None:
                file = _sys.stdout
            self._print_message(self.format_help(), file)

        def _print_message(self, message, file=None):
            if message:
                file = file or _sys.stderr
                try:
                    file.write(message)
                except (AttributeError, OSError):
                    pass

        # ===============
        # Exiting methods
        # ===============

        def exit(self, status=0, message=None):
            if message:
                self._print_message(message, _sys.stderr)
            _sys.exit(status)

        def error(self, message):
            """error(message: string)

        Prints a usage message incorporating the message to stderr and
        exits.

        If you override this in a subclass, it should not return -- it
        should either exit or raise an exception.
        """
            self.print_usage(_sys.stderr)
            args = {'prog': self.prog, 'message': message}
            self.exit(2, _('%(prog)s: error: %(message)s\n') % args)

        def _warning(self, message):
            args = {'prog': self.prog, 'message': message}
            self._print_message(_('%(prog)s: warning: %(message)s\n') % args, _sys.stderr)


if sys._compiled:
    # Compiled programs: the same parsing, messages and help text. parse_args() gives a Namespace
    # whose fields the compiler writes from the program's add_argument() calls (a typed field per
    # destination: type= int / float / str, the actions store / store_true / store_false /
    # store_const / append / append_const / count / extend / help / version); groups, mutually
    # exclusive groups and subparsers too. Not here: type= other functions, set_defaults() of
    # non-literal values, parse_intermixed_args, fromfile_prefix_chars, custom Action classes.
    import os as _os
    import textwrap as _textwrap
    from typing import TypeVar

    _T = TypeVar("_T")
    _N = TypeVar("_N")
    _C = TypeVar("_C")
    _D = TypeVar("_D")
    _H = TypeVar("_H")

    SUPPRESS = "==SUPPRESS=="
    OPTIONAL = "?"
    ZERO_OR_MORE = "*"
    ONE_OR_MORE = "+"
    PARSER = "A..."
    REMAINDER = "..."

    class ArgumentError(Exception):
        """An error from creating or using an argument (optional or positional)."""

        def __init__(self, argument_name: str | None, message: str) -> None:
            self.argument_name = argument_name
            self.message = message
            if argument_name is None:
                super().__init__(message)
            else:
                super().__init__("argument " + argument_name + ": " + message)

    class ArgumentTypeError(Exception):
        """An error from converting a string to a value."""

    class _Exit(Exception):
        def __init__(self, status: int) -> None:
            super().__init__(str(status))
            self.status = status

    class Action:
        """One argument: what parse_args() does with it."""

        def __init__(self, option_strings: list[str], dest: str, kind: str, nargs: str, conv: str,
                     choices: list[str] | None, choice_reprs: list[str] | None, required: bool, help: str | None,
                     metavar: str | None, default: str | None, version: str | None) -> None:
            self.option_strings = option_strings
            self.dest = dest
            self.kind = kind
            self.nargs = nargs
            self.conv = conv
            self.choices = choices
            self.choice_reprs = choice_reprs
            self.required = required
            self.help = help
            self.metavar = metavar
            self.default = default
            self.version = version
            self.group: _MutexGroup | None = None
            self.parser: ArgumentParser | None = None
            self.sub: _SubParsersAction | None = None

        def takes(self) -> bool:
            """Whether it takes values (store, append, extend)."""
            return self.kind == "store" or self.kind == "append" or self.kind == "extend"

        def name(self) -> str:
            """As errors name it: -n/--count, or its metavar / dest."""
            if self.option_strings:
                return "/".join(self.option_strings)
            if self.metavar is not None and self.metavar != SUPPRESS:
                return self.metavar
            if self.dest != SUPPRESS:
                return self.dest
            return ""

        def format_usage(self) -> str:
            return self.option_strings[0]

    class _Parsed:
        """What parsing found: per destination its strings (checked) and how often it was given."""

        def __init__(self) -> None:
            self.vals: dict[str, list[str]] = {}
            self.counts: dict[str, int] = {}
            self.dests: list[str] = []
            self.extras: list[str] = []
            self.defaults: dict[str, str] = {}

        def seen(self, dest: str) -> bool:
            return dest in self.counts

        def count(self, dest: str) -> int:
            return self.counts.get(dest, 0)

        def has(self, dest: str) -> bool:
            return dest in self.dests

        def get_str(self, dest: str) -> str:
            return self.vals[dest][-1]

        def get_int(self, dest: str) -> int:
            return int(self.vals[dest][-1])

        def get_float(self, dest: str) -> float:
            return float(self.vals[dest][-1])

        def get_strs(self, dest: str) -> list[str]:
            return list(self.vals.get(dest, []))

        def get_ints(self, dest: str) -> list[int]:
            return [int(v) for v in self.vals.get(dest, [])]

        def get_floats(self, dest: str) -> list[float]:
            return [float(v) for v in self.vals.get(dest, [])]

        def dflt_str(self, dest: str) -> str:
            return self.defaults[dest]

        def dflt_int(self, dest: str) -> int:
            return int(self.defaults[dest])

        def dflt_float(self, dest: str) -> float:
            return float(self.defaults[dest])

        def none(self, dest: str) -> bool:
            """Given, but with no value (nargs='?' without one)."""
            v = self.vals.get(dest)
            return v is None or len(v) == 0

    def _conv_name(t: _T) -> str:
        if t is None:
            return "str"
        else:
            return t.__name__

    def _nargs_str(n: _N) -> str:
        if n is None:
            return ""
        elif isinstance(n, int):
            return str(n)
        else:
            return str(n)

    def _choice_strs(ch: _C) -> list[str] | None:
        if ch is None:
            return None
        else:
            out: list[str] = []
            for c in ch:
                out.append(str(c))
            return out

    def _choice_reprs(ch: _C) -> list[str] | None:
        if ch is None:
            return None
        else:
            out: list[str] = []
            for c in ch:
                out.append(repr(str(c)))
            return out

    def _default_str(d: _D) -> str | None:
        if d is None:
            return None
        else:
            return str(d)

    class _Group:
        """An argument group: a section of the help."""

        def __init__(self, parser: "ArgumentParser", title: str | None, description: str | None) -> None:
            self.parser = parser
            self.title = title
            self.description = description
            self.actions: list[Action] = []

        def add_argument(self, *name_or_flags: str, action: str = "store", nargs: _N = None, const: _C = None,
                         default: _D = None, type: _T = None, choices: _H = None, required: bool | None = None,
                         help: str | None = None, metavar: str | None = None, dest: str | None = None,
                         version: str | None = None, deprecated: bool = False) -> Action:
            a = self.parser._make_action(list(name_or_flags), action, _nargs_str(nargs), _conv_name(type),
                                         _choice_strs(choices), _choice_reprs(choices), required, help, metavar,
                                         _default_str(default), dest, version)
            self.actions.append(a)
            return a

    class _MutexGroup:
        """Arguments of which one at most may be given (required: exactly one)."""

        def __init__(self, parser: "ArgumentParser", required: bool, group: _Group) -> None:
            self.parser = parser
            self.required = required
            self.group = group
            self.actions: list[Action] = []

        def add_argument(self, *name_or_flags: str, action: str = "store", nargs: _N = None, const: _C = None,
                         default: _D = None, type: _T = None, choices: _H = None, required: bool | None = None,
                         help: str | None = None, metavar: str | None = None, dest: str | None = None,
                         version: str | None = None, deprecated: bool = False) -> Action:
            a = self.parser._make_action(list(name_or_flags), action, _nargs_str(nargs), _conv_name(type),
                                         _choice_strs(choices), _choice_reprs(choices), required, help, metavar,
                                         _default_str(default), dest, version)
            if a.required:
                raise ValueError("mutually exclusive arguments must be optional")
            a.group = self
            self.actions.append(a)
            self.group.actions.append(a)
            return a

    class _SubParsersAction:
        """parser.add_subparsers(): a command chosen by name, each its own parser."""

        def __init__(self, parser: "ArgumentParser", action: Action, prog: str) -> None:
            self.parser = parser
            self.action = action
            self.prog = prog
            self.names: list[str] = []
            self.parsers: list[ArgumentParser] = []
            self.helps: list[str | None] = []

        def add_parser(self, name: str, *, help: str | None = None, aliases: list[str] | None = None,
                       description: str | None = None, epilog: str | None = None, prog: str | None = None,
                       add_help: bool = True, usage: str | None = None, deprecated: bool = False) -> "ArgumentParser":
            p = ArgumentParser(prog=prog if prog is not None else self.prog + " " + name, description=description,
                               epilog=epilog, add_help=add_help, usage=usage)
            p._exit_on_error = self.parser._exit_on_error
            self.names.append(name)
            self.parsers.append(p)
            self.helps.append(help)
            if aliases is not None:
                for al in aliases:
                    self.names.append(al)
                    self.parsers.append(p)
                    self.helps.append(None)
            choices: list[str] = []
            reprs: list[str] = []
            for n in self.names:
                choices.append(n)
                reprs.append(repr(n))
            self.action.choices = choices
            self.action.choice_reprs = reprs
            return p

    def _columns() -> int:
        c = _os.environ.get("COLUMNS")
        if c is not None:
            try:
                return int(c)
            except ValueError:
                pass
        return 80

    class ArgumentParser:
        """Turns command-line arguments into a Namespace: add_argument(), then parse_args()."""

        def __init__(self, prog: str | None = None, usage: str | None = None, description: str | None = None,
                     epilog: str | None = None, parents: list["ArgumentParser"] | None = None, formatter_class: _H = None,
                     prefix_chars: str = "-", fromfile_prefix_chars: str | None = None, argument_default: _D = None,
                     conflict_handler: str = "error", add_help: bool = True, allow_abbrev: bool = True,
                     exit_on_error: bool = True, *, suggest_on_error: bool = False, color: bool = True) -> None:
            if prog is None:
                a0 = sys.argv[0] if len(sys.argv) > 0 else "prog"
                i = a0.rfind("/")
                prog = a0[i + 1:] if i >= 0 else a0
            self.prog = prog
            self.usage = usage
            self.description = description
            self.epilog = epilog
            self.add_help = add_help
            self.allow_abbrev = allow_abbrev
            self._exit_on_error = exit_on_error
            self.prefix_chars = prefix_chars
            self._actions: list[Action] = []
            self._groups: list[_Group] = []
            self._mutex: list[_MutexGroup] = []
            self._positionals = _Group(self, "positional arguments", None)
            self._optionals = _Group(self, "options", None)
            self._groups.append(self._positionals)
            self._groups.append(self._optionals)
            self._sub: _SubParsersAction | None = None
            self._negative_like = False
            if add_help:
                h = self._make_action(["-h", "--help"], "help", "", "str", None, None, None,
                                      "show this help message and exit", None, None, None, None)
                self._optionals.actions.append(h)
            if parents is not None:
                for par in parents:
                    for a in par._actions:
                        if a.kind == "help":
                            continue
                        self._actions.append(a)
                        if a.option_strings:
                            self._optionals.actions.append(a)
                        else:
                            self._positionals.actions.append(a)

        def _make_action(self, flags: list[str], kind: str, nargs: str, conv: str, choices: list[str] | None,
                         reprs: list[str] | None, required: bool | None, help: str | None, metavar: str | None,
                         default: str | None, dest: str | None, version: str | None) -> Action:
            if not flags and dest is None:
                raise ValueError("dest supplied twice for positional argument")
            options: list[str] = []
            d = dest
            positional = len(flags) > 0 and not flags[0].startswith("-")
            if positional:
                if len(flags) > 1:
                    raise ValueError("invalid option string " + repr(flags[0]) + ": must start with a character '-'")
                if dest is not None:
                    raise ValueError("dest supplied twice for positional argument")
                d = flags[0]
            else:
                long_d: str | None = None
                for f in flags:
                    if not f.startswith("-"):
                        raise ValueError("invalid option string " + repr(f) + ": must start with a character '-'")
                    options.append(f)
                    if long_d is None and f.startswith("--") and len(f) > 2:
                        long_d = f[2:]
                    if len(f) > 1 and (f[1:].replace(".", "", 1).isdigit()):
                        self._negative_like = True
                if d is None:
                    d = long_d if long_d is not None else options[0][1:]
                    d = d.replace("-", "_")
            if kind == "store_true" or kind == "store_false" or kind == "store_const" or kind == "append_const" \
                    or kind == "count" or kind == "help" or kind == "version":
                n = "0"
            else:
                n = nargs
            if n == "0" and kind in ("store", "append", "extend"):
                raise ValueError("nargs for store actions must be != 0; if you have nothing to store, actions such as store true or store const may be more appropriate")
            if kind == "help" or kind == "version":
                d = SUPPRESS
            req = required if required is not None else False
            if positional and required is None:
                req = n != "?" and n != "*"
            if positional and required is not None:
                raise TypeError("'required' is an invalid argument for positionals")
            if kind not in ("store", "store_const", "store_true", "store_false", "append", "append_const", "count",
                            "help", "version", "extend"):
                raise ValueError("unknown action " + repr(kind))
            a = Action(options, d if d is not None else "", kind, n, conv, choices, reprs, req, help, metavar,
                       default, version)
            a.parser = self
            for b in self._actions:
                for o in options:
                    if o in b.option_strings:
                        raise ArgumentError(a.name(), "conflicting option string: " + o)
            self._actions.append(a)
            return a

        def add_argument(self, *name_or_flags: str, action: str = "store", nargs: _N = None, const: _C = None,
                         default: _D = None, type: _T = None, choices: _H = None, required: bool | None = None,
                         help: str | None = None, metavar: str | None = None, dest: str | None = None,
                         version: str | None = None, deprecated: bool = False) -> Action:
            """An argument: positional ("name") or optional ("-n", "--count")."""
            a = self._make_action(list(name_or_flags), action, _nargs_str(nargs), _conv_name(type),
                                  _choice_strs(choices), _choice_reprs(choices), required, help, metavar,
                                  _default_str(default), dest, version)
            if a.option_strings:
                self._optionals.actions.append(a)
            else:
                self._positionals.actions.append(a)
            return a

        def add_argument_group(self, title: str | None = None, description: str | None = None) -> _Group:
            g = _Group(self, title, description)
            self._groups.append(g)
            return g

        def add_mutually_exclusive_group(self, *, required: bool = False) -> _MutexGroup:
            m = _MutexGroup(self, required, self._optionals)
            self._mutex.append(m)
            return m

        def add_subparsers(self, *, title: str | None = None, description: str | None = None, prog: str | None = None,
                           dest: str | None = None, required: bool = False, help: str | None = None,
                           metavar: str | None = None) -> _SubParsersAction:
            if self._sub is not None:
                raise ArgumentError(None, "cannot have multiple subparser arguments")
            a = Action([], dest if dest is not None else SUPPRESS, "parsers", "A...", "str", [], [], required, help,
                       metavar, None, None)
            a.parser = self
            self._actions.append(a)
            if title is not None or description is not None:
                g = self.add_argument_group(title if title is not None else "subcommands", description)
                g.actions.append(a)
            else:
                self._positionals.actions.append(a)
            pp = prog
            if pp is None:
                parts = [self.prog]
                for b in self._actions:
                    if not b.option_strings and b.kind != "parsers":
                        parts.append(self._format_args(b, b.dest))
                pp = " ".join(parts)
            s = _SubParsersAction(self, a, pp)
            a.sub = s
            self._sub = s
            return s

        def set_defaults(self, **kwargs: _D) -> None:
            pass

        def get_default(self, dest: str) -> str | None:
            for a in self._actions:
                if a.dest == dest:
                    return a.default
            return None

        # ---------------------------------------------------------------- parsing

        def parse_args(self, args: list[str] | None = None, namespace: _D = None) -> "Namespace":
            """The arguments (sys.argv[1:] by default) as a Namespace; errors: usage and exit(2)."""
            p = self._parse_known(args)
            if p.extras:
                self.error("unrecognized arguments: " + " ".join(p.extras))
            return Namespace(p)

        def parse_known_args(self, args: list[str] | None = None, namespace: _D = None) -> tuple["Namespace", list[str]]:
            p = self._parse_known(args)
            return Namespace(p), p.extras

        def _parse_known(self, args: list[str] | None) -> _Parsed:
            argv = list(args) if args is not None else sys.argv[1:]
            p = _Parsed()
            try:
                self._parse_into(argv, p)
            except ArgumentError as e:
                if self._exit_on_error:
                    self.error(str(e))
                raise
            return p

        def _is_option(self, s: str) -> bool:
            if not s or s[0] not in self.prefix_chars or s == "-":
                return False
            if s[1:2].isdigit() or (s[1:2] == "." and s[2:3].isdigit()):
                try:
                    float(s)
                    if not self._negative_like:
                        return False
                except ValueError:
                    pass
            if " " in s and "=" not in s.split(" ")[0]:
                return False
            return True

        def _find_option(self, s: str) -> tuple[Action | None, str | None, str]:
            """The action of option string s, its explicit value (--x=v, -xv), and the option as written."""
            for a in self._actions:
                if s in a.option_strings:
                    return a, None, s
            if "=" in s:
                o, v = s.split("=", 1)
                for a in self._actions:
                    if o in a.option_strings:
                        return a, v, o
            if s.startswith("--") and self.allow_abbrev:
                o = s.split("=", 1)[0]
                v: str | None = s.split("=", 1)[1] if "=" in s else None
                found: list[tuple[Action, str]] = []
                for a in self._actions:
                    for f in a.option_strings:
                        if f.startswith(o):
                            found.append((a, f))
                if len(found) == 1:
                    return found[0][0], v, found[0][1]
                if len(found) > 1:
                    names = ", ".join([f for a, f in found])
                    raise ArgumentError(None, "ambiguous option: " + o + " could match " + names)
            elif len(s) > 2 and not s.startswith("--"):
                o = s[:2]
                for a in self._actions:
                    if o in a.option_strings:
                        return a, s[2:], o
            return None, None, s

        def _check_value(self, a: Action, v: str) -> str:
            """v converted (checked) and kept as text; its choice checked."""
            out = v
            if a.conv == "int":
                try:
                    out = str(int(v))
                except ValueError:
                    raise ArgumentError(a.name(), "invalid int value: " + repr(v))
            elif a.conv == "float":
                try:
                    out = repr(float(v))
                except ValueError:
                    raise ArgumentError(a.name(), "invalid float value: " + repr(v))
            elif a.conv != "str":
                raise ArgumentError(a.name(), "invalid " + a.conv + " value: " + repr(v))
            ch = a.choices
            if ch is not None and a.kind != "parsers":
                key = out
                if a.conv == "float":
                    key = str(float(v))
                if key not in ch:
                    shown = repr(v) if a.conv == "str" else repr(key)
                    raise ArgumentError(a.name(), "invalid choice: " + shown + " (choose from " +
                                        ", ".join(a.choice_reprs if a.choice_reprs is not None else ch) + ")")
            return out

        def _take(self, a: Action, values: list[str], p: _Parsed) -> None:
            """The action done with its values."""
            for b in self._actions:                     # mutually exclusive
                pass
            if a.group is not None:
                for other in a.group.actions:
                    if other is not a and other.dest in p.counts and other.dest != a.dest:
                        raise ArgumentError(a.name(), "not allowed with argument " + other.name())
            if a.kind == "help":
                self.print_help()
                self.exit(0)
            if a.kind == "version":
                v = a.version if a.version is not None else ""
                print(v.replace("%(prog)s", self.prog))
                self.exit(0)
            checked = [self._check_value(a, v) for v in values]
            if a.kind == "store":
                p.vals[a.dest] = checked
            elif a.kind == "append" or a.kind == "extend":
                if a.dest not in p.vals:
                    p.vals[a.dest] = []
                p.vals[a.dest].extend(checked)
            p.counts[a.dest] = p.counts.get(a.dest, 0) + 1

        def _consume(self, a: Action, explicit: str | None, rest: list[str], i: int) -> int:
            """The values option a takes from rest[i:] (and its explicit one): how many it took."""
            n = a.nargs
            if n == "0":
                if explicit is not None:
                    raise ArgumentError(a.name(), "ignored explicit argument " + repr(explicit))
                return 0
            if explicit is not None:
                if n == "" or n == "?" or n == "1":
                    return -1
                if n == "*" or n == "+":
                    return -1
                raise ArgumentError(a.name(), "expected " + n + " arguments")
            avail = 0
            while i + avail < len(rest) and rest[i + avail] != "--" and not self._is_option(rest[i + avail]):
                avail += 1
            if n == "":
                if avail < 1:
                    raise ArgumentError(a.name(), "expected one argument")
                return 1
            if n == "?":
                return 1 if avail >= 1 else 0
            if n == "*":
                return avail
            if n == "+":
                if avail < 1:
                    raise ArgumentError(a.name(), "expected at least one argument")
                return avail
            k = int(n)
            if avail < k:
                raise ArgumentError(a.name(), "expected " + n + " argument" + ("s" if k != 1 else ""))
            return k

        def _parse_into(self, argv: list[str], p: _Parsed) -> None:
            for a in self._actions:
                if a.dest != SUPPRESS and a.dest not in p.dests:
                    p.dests.append(a.dest)
                d = a.default
                if d is not None and a.dest not in p.defaults:
                    p.defaults[a.dest] = d
            positionals: list[str] = []
            i = 0
            only_pos = False
            sub_at = -1
            while i < len(argv):
                s = argv[i]
                if not only_pos and s == "--":
                    only_pos = True
                    i += 1
                    continue
                if only_pos or not self._is_option(s):
                    if self._sub is not None and sub_at < 0 and self._positionals_before_sub() <= len(positionals):
                        sub_at = len(positionals)
                        positionals.append(s)
                        positionals.extend(argv[i + 1:])
                        break
                    positionals.append(s)
                    i += 1
                    continue
                a, explicit, written = self._find_option(s)
                if a is not None and a.nargs == "0" and explicit is not None and len(written) == 2 and s[1] != "-":
                    self._combined(s, p)                     # -qq, -vq: flags together
                    i += 1
                    continue
                if a is None:
                    if len(s) > 2 and s[0] == "-" and s[1] != "-":    # -vq: several flags
                        if self._combined(s, p):
                            i += 1
                            continue
                    p.extras.append(s)
                    i += 1
                    continue
                took = self._consume(a, explicit, argv, i + 1)
                if took == -1:
                    vals = [explicit if explicit is not None else ""]
                    took = 0
                else:
                    vals = argv[i + 1:i + 1 + took]
                self._take(a, vals, p)
                i += 1 + took
            self._match_positionals(positionals, sub_at, p)
            missing: list[str] = []
            for a in self._actions:
                if a.required and a.dest not in p.counts and a.kind != "parsers":
                    missing.append(a.name())
                if a.kind == "parsers" and a.required and a.dest not in p.counts:
                    missing.append(a.name() if a.dest != SUPPRESS else "{" + ",".join(a.choices or []) + "}")
            if missing:
                raise ArgumentError(None, "the following arguments are required: " + ", ".join(missing))
            for m in self._mutex:
                if m.required:
                    if not any(b.dest in p.counts for b in m.actions):
                        names = [b.name() for b in m.actions if b.help != SUPPRESS]
                        raise ArgumentError(None, "one of the arguments " + " ".join(names) + " is required")

        def _combined(self, s: str, p: _Parsed) -> bool:
            """-vq: short flags together (the last may take the rest as its value)."""
            acts: list[Action] = []
            for k in range(1, len(s)):
                o = "-" + s[k]
                found: Action | None = None
                for a in self._actions:
                    if o in a.option_strings:
                        found = a
                if found is None:
                    if not acts:
                        return False
                    raise ArgumentError(acts[-1].name(), "ignored explicit argument " + repr(s[k:]))
                if found.nargs != "0":
                    if acts:
                        for b in acts:
                            self._take(b, [], p)
                    self._take(found, [s[k + 1:]] if k + 1 < len(s) else [], p)
                    return True
                acts.append(found)
            for b in acts:
                self._take(b, [], p)
            return True

        def _positionals_before_sub(self) -> int:
            n = 0
            for a in self._actions:
                if not a.option_strings and a.kind != "parsers":
                    n += 1
            return n

        def _match_positionals(self, args: list[str], sub_at: int, p: _Parsed) -> None:
            acts = [a for a in self._actions if not a.option_strings]
            mins: list[int] = []
            for a in acts:
                n = a.nargs
                if n == "" or n == "+":
                    mins.append(1)
                elif n == "?" or n == "*" or n == "...":
                    mins.append(0)
                elif n == "A...":
                    mins.append(1)
                else:
                    mins.append(int(n))
            i = 0
            for k in range(len(acts)):
                a = acts[k]
                if a.kind == "parsers":
                    if i < len(args):
                        name = args[i]
                        sub = a.sub
                        if sub is None or name not in sub.names:
                            raise ArgumentError(a.name() if a.dest != SUPPRESS else None,
                                                "invalid choice: " + repr(name) + " (choose from " +
                                                ", ".join(a.choice_reprs or []) + ")")
                        if a.dest != SUPPRESS:
                            p.vals[a.dest] = [name]
                            p.counts[a.dest] = 1
                        child = sub.parsers[sub.names.index(name)]
                        child._parse_into(args[i + 1:], p)
                        i = len(args)
                    continue
                left = len(args) - i
                need_after = 0
                for j in range(k + 1, len(acts)):
                    need_after += mins[j]
                n = a.nargs
                if n == "":
                    take = 1
                elif n == "?":
                    take = 1 if left - need_after >= 1 else 0
                elif n == "*" or n == "...":
                    take = max(0, left - need_after)
                elif n == "+":
                    take = max(1, left - need_after)
                else:
                    take = int(n)
                if take > left:
                    continue
                vals = args[i:i + take]
                if take == 0 and n == "?":
                    p.counts[a.dest] = p.counts.get(a.dest, 0)
                    i += 0
                    continue
                if take == 0 and (n == "*" or n == "..."):
                    p.vals[a.dest] = []
                    p.counts[a.dest] = 1
                    continue
                self._take(a, vals, p)
                i += take
            p.extras.extend(args[i:])

        # ---------------------------------------------------------------- help

        def _metavar(self, a: Action, default: str) -> str:
            if a.metavar is not None:
                return a.metavar
            if a.choices is not None:
                return "{" + ",".join(a.choices) + "}"
            return default

        def _format_args(self, a: Action, default: str) -> str:
            m = self._metavar(a, default)
            n = a.nargs
            if n == "":
                return m
            if n == "?":
                return "[" + m + "]"
            if n == "*":
                return "[" + m + " ...]"
            if n == "+":
                return m + " [" + m + " ...]"
            if n == "...":
                return "..."
            if n == "A...":
                return m + " ..."
            if n == "0":
                return ""
            return " ".join([m] * int(n))

        def _invocation(self, a: Action) -> str:
            if not a.option_strings:
                return self._metavar(a, a.dest)
            if a.nargs == "0":
                return ", ".join(a.option_strings)
            return ", ".join(a.option_strings) + " " + self._format_args(a, a.dest.upper())

        def _usage_parts(self) -> tuple[list[str], int]:
            opt_parts: list[str] = []
            done: list[_MutexGroup] = []
            for a in self._actions:
                if not a.option_strings or a.help == SUPPRESS:
                    continue
                g = a.group
                if g is not None:
                    if g in done:
                        continue
                    done.append(g)
                    inner: list[str] = []
                    for b in g.actions:
                        if b.help == SUPPRESS:
                            continue
                        inner.append(b.option_strings[0] if b.nargs == "0" else
                                     b.option_strings[0] + " " + self._format_args(b, b.dest.upper()))
                    if inner:
                        opt_parts.append(("(" if g.required else "[") + " | ".join(inner) + (")" if g.required else "]"))
                    continue
                part = a.option_strings[0] if a.nargs == "0" else a.option_strings[0] + " " + self._format_args(a, a.dest.upper())
                if not a.required:
                    part = "[" + part + "]"
                opt_parts.append(part)
            pos_parts: list[str] = []
            for a in self._actions:
                if a.option_strings or a.help == SUPPRESS:
                    continue
                pos_parts.append(self._format_args(a, a.dest))
            return opt_parts + [x for x in pos_parts if x], len(opt_parts)

        def format_usage(self) -> str:
            return self._format_usage(_columns() - 2)

        def _format_usage(self, width: int) -> str:
            prefix = "usage: "
            if self.usage is not None:
                return prefix + self.usage.replace("%(prog)s", self.prog) + "\n"
            parts, pos_start = self._usage_parts()
            prog = self.prog
            usage = " ".join([prog] + parts)
            text_width = width
            if len(prefix) + len(usage) > text_width:
                opt_parts = parts[:pos_start]
                pos_parts = parts[pos_start:]
                if len(prefix) + len(prog) <= 0.75 * text_width:
                    indent = " " * (len(prefix) + len(prog) + 1)
                    if opt_parts:
                        lines = _get_lines([prog] + opt_parts, indent, text_width, prefix)
                        lines.extend(_get_lines(pos_parts, indent, text_width, None))
                    elif pos_parts:
                        lines = _get_lines([prog] + pos_parts, indent, text_width, prefix)
                    else:
                        lines = [prog]
                else:
                    indent = " " * len(prefix)
                    ps = opt_parts + pos_parts
                    lines = _get_lines(ps, indent, text_width, None)
                    if len(lines) > 1:
                        lines = []
                        lines.extend(_get_lines(opt_parts, indent, text_width, None))
                        lines.extend(_get_lines(pos_parts, indent, text_width, None))
                    lines = [prog] + lines
                usage = "\n".join(lines)
            return prefix + usage + "\n"

        def format_help(self) -> str:
            width = _columns() - 2
            out = self._format_usage(width) + "\n"
            if self.description:
                out += _fill(self.description, width, "") + "\n\n"
            max_help = min(24, max(width - 20, 4))
            longest = 0
            for g in self._groups:
                for a in g.actions:
                    if a.help == SUPPRESS:
                        continue
                    longest = max(longest, len(self._invocation(a)) + 2)
                    sub = a.sub
                    if sub is not None:
                        for k in range(len(sub.names)):
                            if sub.helps[k] is not None:
                                longest = max(longest, len(sub.names[k]) + 4)
            help_pos = min(longest + 2, max_help)
            for g in self._groups:
                shown = [a for a in g.actions if a.help != SUPPRESS]
                if not shown and not g.description:
                    continue
                out += (g.title if g.title is not None else "") + ":\n"
                if g.description:
                    out += _fill(g.description, width - 2, "  ") + "\n\n"
                for a in shown:
                    out += self._format_action(a, 2, help_pos, width)
                    sub = a.sub
                    if sub is not None:
                        for k in range(len(sub.names)):
                            h = sub.helps[k]
                            if h is not None:
                                out += self._format_entry(sub.names[k], h, 4, help_pos, width)
                out += "\n"
            if self.epilog:
                out += _fill(self.epilog, width, "") + "\n"
            return out.rstrip("\n") + "\n"

        def _format_action(self, a: Action, indent: int, help_pos: int, width: int) -> str:
            return self._format_entry(self._invocation(a), a.help, indent, help_pos, width, a)

        def _format_entry(self, header: str, help: str | None, indent: int, help_pos: int, width: int,
                          a: Action | None = None) -> str:
            help_width = max(width - help_pos, 11)
            action_width = help_pos - indent - 2
            if not help:
                return " " * indent + header + "\n"
            if len(header) <= action_width:
                out = " " * indent + header + " " * (action_width - len(header)) + "  "
                first = 0
            else:
                out = " " * indent + header + "\n"
                first = help_pos
            text = help
            if "%(" in text:
                text = text.replace("%(prog)s", self.prog)
                if a is not None:
                    text = text.replace("%(default)s", str(a.default)).replace("%(dest)s", a.dest)
                    if a.choices is not None:
                        text = text.replace("%(choices)s", ", ".join(a.choices))
                    text = text.replace("%(metavar)s", str(a.metavar))
            lines = _textwrap.wrap(" ".join(text.split()), help_width)
            if not lines:
                return out.rstrip(" ") + "\n"
            out += " " * first + lines[0] + "\n"
            for line in lines[1:]:
                out += " " * help_pos + line + "\n"
            return out

        def print_usage(self, file: _D = None) -> None:
            print(self.format_usage(), end="")

        def print_help(self, file: _D = None) -> None:
            print(self.format_help(), end="")

        def exit(self, status: int = 0, message: str | None = None) -> None:
            if message:
                print(message, end="", file=sys.stderr)
            raise SystemExit(status)

        def error(self, message: str) -> None:
            """Prints the usage and the message to stderr, then exits with status 2."""
            print(self.format_usage() + self.prog + ": error: " + message, file=sys.stderr)
            raise SystemExit(2)

    def _get_lines(parts: list[str], indent: str, text_width: int, prefix: str | None) -> list[str]:
        lines: list[str] = []
        line: list[str] = []
        indent_length = len(indent)
        line_len = len(prefix) - 1 if prefix is not None else indent_length - 1
        for part in parts:
            if line_len + 1 + len(part) > text_width and line:
                lines.append(indent + " ".join(line))
                line = []
                line_len = indent_length - 1
            line.append(part)
            line_len += len(part) + 1
        if line:
            lines.append(indent + " ".join(line))
        if prefix is not None:
            lines[0] = lines[0][indent_length:]
        return lines

    def _fill(text: str, width: int, indent: str) -> str:
        return _textwrap.fill(" ".join(text.split()), width, initial_indent=indent, subsequent_indent=indent)

    class HelpFormatter:
        """(The formatter_class: compiled programs format help as HelpFormatter does.)"""

    class RawDescriptionHelpFormatter(HelpFormatter):
        pass

    class RawTextHelpFormatter(RawDescriptionHelpFormatter):
        pass

    class ArgumentDefaultsHelpFormatter(HelpFormatter):
        pass

    class _NamespaceBase:
        """parse_args()'s result: the compiler writes the Namespace class (one field per argument)."""
