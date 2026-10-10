"""Topological sorting of graphs (CPython's graphlib): TopologicalSorter, CycleError.

Compiled programs: a sorter's nodes are of one type; TopologicalSorter(graph) takes a dict of sets."""
import sys
from typing import Generic, TypeVar

__all__ = ["TopologicalSorter", "CycleError"]

_T = TypeVar("_T")

_NODE_OUT = -1
_NODE_DONE = -2


class _NodeInfo(Generic[_T]):
    def __init__(self, node: _T) -> None:
        # The node this class is augmenting.
        self.node = node

        # Number of predecessors, generally >= 0. When this value falls to 0,
        # and is returned by get_ready(), this is set to _NODE_OUT and when the
        # node is marked done by a call to done(), set to _NODE_DONE.
        self.npredecessors = 0

        # List of successor nodes. The list can contain duplicated elements as
        # long as they're all reflected in the successor's npredecessors attribute.
        self.successors: list[_T] = []


if not sys._compiled:
    class CycleError(ValueError):
        """Raised by TopologicalSorter.prepare if cycles exist in the working graph: its args are
        ("nodes are in a cycle", [the nodes of one cycle, the first again at the end])."""
        pass

if sys._compiled:
    class CycleError(ValueError):
        """Raised by TopologicalSorter.prepare if cycles exist in the working graph: its args are
        ("nodes are in a cycle", [the nodes of one cycle, the first again at the end]) - in compiled
        programs the nodes' str()."""

        def __init__(self, message: str, cycle: list[str]) -> None:
            super().__init__(message)
            self.args = (message, cycle)

        def __str__(self) -> str:
            return repr(self.args)


class TopologicalSorter(Generic[_T]):
    """Provides functionality to topologically sort a graph of hashable nodes"""

    def __init__(self, graph: dict[_T, set[_T]] | None = None) -> None:
        self._node2info = {}            # node -> _NodeInfo (compiled: unannotated, as a generic in a generic)
        self._ready_nodes: list[_T] | None = None
        self._npassedout = 0
        self._nfinished = 0

        if graph is not None:
            for node, predecessors in graph.items():
                self.add(node, *list(predecessors))

    def _get_nodeinfo(self, node: _T):
        result = self._node2info.get(node)
        if result is None:
            result = _NodeInfo(node)
            self._node2info[node] = result
        return result

    def add(self, node: _T, *predecessors: _T) -> None:
        """Add a new node and its predecessors to the graph (more calls for a node: the union)."""
        if self._ready_nodes is not None:
            raise ValueError("Nodes cannot be added after a call to prepare()")

        # Create the node -> predecessor edges
        nodeinfo = self._get_nodeinfo(node)
        nodeinfo.npredecessors += len(predecessors)

        # Create the predecessor -> node edges
        for pred in predecessors:
            pred_info = self._get_nodeinfo(pred)
            pred_info.successors.append(node)

    def prepare(self) -> None:
        """Mark the graph as finished and check for cycles in the graph (CycleError)."""
        if self._npassedout > 0:
            raise ValueError("cannot prepare() after starting sort")

        if self._ready_nodes is None:
            self._ready_nodes = [i.node for i in self._node2info.values() if i.npredecessors == 0]
        # ready_nodes is set before we look for cycles on purpose:
        # if the user wants to catch the CycleError, that's fine,
        # they can continue using the instance to grab as many
        # nodes as possible before cycles block more progress
        cycle = self._find_cycle()
        if cycle:
            if sys._compiled:
                raise CycleError("nodes are in a cycle", [str(n) for n in cycle])
            raise CycleError("nodes are in a cycle", cycle)

    def get_ready(self) -> tuple[_T, ...]:
        """Return a tuple of all the nodes that are ready (once none can be: an empty tuple)."""
        ready = self._ready_nodes
        if ready is None:
            raise ValueError("prepare() must be called first")

        # Get the nodes that are ready and mark them
        result = tuple(ready)
        n2i = self._node2info
        for node in result:
            n2i[node].npredecessors = _NODE_OUT

        # Clean the list of nodes that are ready and update
        # the counter of nodes that we have returned.
        ready.clear()
        self._npassedout += len(result)

        return result

    def is_active(self) -> bool:
        """Return True if more progress can be made and False otherwise."""
        ready = self._ready_nodes
        if ready is None:
            raise ValueError("prepare() must be called first")
        return self._nfinished < self._npassedout or bool(ready)

    def __bool__(self) -> bool:
        return self.is_active()

    def done(self, *nodes: _T) -> None:
        """Marks a set of nodes returned by "get_ready" as processed."""
        ready = self._ready_nodes
        if ready is None:
            raise ValueError("prepare() must be called first")

        n2i = self._node2info

        for node in nodes:

            # Check if we know about this node (it was added previously using add()
            nodeinfo = n2i.get(node)
            if nodeinfo is None:
                raise ValueError(f"node {node!r} was not added using add()")

            # If the node has not being returned (marked as ready) previously, inform the user.
            stat = nodeinfo.npredecessors
            if stat != _NODE_OUT:
                if stat >= 0:
                    raise ValueError(f"node {node!r} was not passed out (still not ready)")
                elif stat == _NODE_DONE:
                    raise ValueError(f"node {node!r} was already marked done")
                else:
                    raise AssertionError(f"node {node!r}: unknown status {stat}")

            # Mark the node as processed
            nodeinfo.npredecessors = _NODE_DONE

            # Go to all the successors and reduce the number of predecessors, collecting all the ones
            # that are ready to be returned in the next get_ready() call.
            for successor in nodeinfo.successors:
                successor_info = n2i[successor]
                successor_info.npredecessors -= 1
                if successor_info.npredecessors == 0:
                    ready.append(successor)
            self._nfinished += 1

    def _find_cycle(self) -> list[_T] | None:
        n2i = self._node2info
        stack: list[_T] = []
        itstack: list[int] = []             # (the next successor to visit, per node of the stack)
        seen: set[_T] = set()
        node2stacki: dict[_T, int] = {}

        for start in n2i:
            if start in seen:
                continue
            node = start
            while True:
                if node in seen:
                    # If we have seen already the node and is in the
                    # current stack we have found a cycle.
                    if node in node2stacki:
                        return stack[node2stacki[node]:] + [node]
                    # else go on to get next successor
                else:
                    seen.add(node)
                    itstack.append(0)
                    node2stacki[node] = len(stack)
                    stack.append(node)

                # Backtrack to the topmost stack entry with
                # at least another successor.
                found = False
                while stack:
                    succs = n2i[stack[-1]].successors
                    k = itstack[-1]
                    if k < len(succs):
                        itstack[-1] = k + 1
                        node = succs[k]
                        found = True
                        break
                    del node2stacki[stack.pop()]
                    itstack.pop()
                if not found:
                    break
        return None

    def static_order(self):
        """Returns an iterable of nodes in a topological order (prepare() and done() are called for it)."""
        self.prepare()
        while self.is_active():
            node_group = self.get_ready()
            yield from node_group
            self.done(*node_group)
