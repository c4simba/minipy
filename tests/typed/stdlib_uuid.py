import uuid
u = uuid.UUID('12345678-1234-5678-1234-567812345678')
print(u, repr(u), u.hex, u.bytes, u.bytes_le, u.fields, u.urn, u.version, u.variant, u.time_low, u.node, u.clock_seq)
print(uuid.uuid3(uuid.NAMESPACE_DNS, 'python.org'), uuid.uuid5(uuid.NAMESPACE_DNS, 'python.org'), uuid.uuid5(uuid.NAMESPACE_URL, b'x'))
print(uuid.UUID('{12345678-1234-5678-1234-567812345678}') == u, uuid.UUID(int=5), uuid.UUID(bytes=bytes(range(16))), uuid.UUID(fields=(1, 2, 3, 4, 5, 6)))
print(uuid.uuid4().version, uuid.uuid1().version, uuid.uuid7().version, uuid.uuid6().version, uuid.uuid8(1, 2, 3))
print(uuid.NIL, uuid.MAX, u < uuid.MAX, hash(u) == hash(uuid.UUID(u.hex)), uuid.SafeUUID.unknown, u.is_safe, uuid.UUID(int=5).int)
for bad in ('xyz', '12345678-1234-5678-1234-56781234567g'):
    try:
        uuid.UUID(bad)
    except ValueError as e:
        print("ValueError:", e)
print(str(uuid.UUID(bytes_le=u.bytes_le)), uuid.RESERVED_NCS, uuid.RFC_4122, uuid.uuid4() != uuid.uuid4(), len(str(uuid.uuid4())))
print(sorted([uuid.MAX, uuid.NIL, u]), uuid.UUID(u.urn) == u, uuid.getnode() == uuid.getnode())
