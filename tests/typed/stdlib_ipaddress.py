# ipaddress: IPv4/IPv6 addresses, networks and interfaces (128-bit values on 64-bit ints)
import ipaddress
from ipaddress import IPv4Address, IPv6Address, IPv4Network, IPv6Network, ip_address, ip_network, ip_interface

ADDRS = ['0.0.0.0', '10.1.2.3', '100.64.1.1', '127.0.0.1', '169.254.3.4', '172.16.5.4', '192.0.0.9',
         '192.0.2.1', '192.168.1.1', '198.51.100.7', '224.0.0.251', '240.1.2.3', '255.255.255.255', '8.8.8.8',
         '::', '::1', '::ffff:127.0.0.1', '::ffff:8.8.8.8', '2001:db8::1', '2001:4860:4860::8888',
         'fe80::1', 'fec0::1', 'ff02::1', 'fc00::1', '2002:c000:204::1', '2001:1::1', '64:ff9b::1.2.3.4']

print("== properties")
for s in ADDRS:
    a = ip_address(s)
    print(s, a.version, a.is_private, a.is_global, a.is_multicast, a.is_reserved, a.is_loopback,
          a.is_link_local, a.is_unspecified, a.exploded, a.compressed)

print("== IPv6 text")
for s in ['2001:0db8:0000:0000:0000:ff00:0042:8329', '2001:db8:0:0:1:0:0:1', '1:0:0:0:0:0:0:0', '0:0:0:0:0:0:0:1',
          '1:2:3:4:5:6:7:8', 'FE80::ABCD', '::ffff:c000:280', '1::', '1:0:0:2::3', '2001:db8:0:0:1::']:
    v = IPv6Address(s)
    print(s, '->', v, v.exploded, v.packed.hex(), repr(v))
print(IPv6Address(1), IPv6Address(2 ** 40), IPv6Address(b'\x20\x01' + bytes(13) + b'\x07'), int(IPv6Address('::ff')))
print(ip_address('::ffff:1.2.3.4').reverse_pointer)
print(ip_address('2001:db8::1').reverse_pointer)
print(IPv6Address('2001:0:4136:e378:8000:63bf:3fff:fdd2').teredo, IPv6Address('2002:c000:204::1').sixtofour,
      IPv6Address('::1').teredo, IPv6Address('::1').sixtofour, IPv6Address('::1').ipv4_mapped)
print(IPv4Address('1.2.3.4').ipv6_mapped, IPv6Address('fe80::1%eth0'), IPv6Address('fe80::1%eth0').scope_id,
      IPv6Address('fe80::1').scope_id, IPv6Address('fe80::1%eth0') == IPv6Address('fe80::1'))

print("== IPv4")
a4 = IPv4Address('192.0.2.1')
print(a4, repr(a4), int(a4), a4.packed, IPv4Address(3221225985), IPv4Address(b'\xc0\x00\x02\x01'), a4.max_prefixlen,
      a4 + 255, a4 - 1, a4 == IPv4Address('192.0.2.1'), a4 != IPv4Address('192.0.2.2'), a4 < IPv4Address('192.0.2.2'),
      a4 >= IPv4Address('192.0.2.2'), a4 <= a4, a4 > IPv4Address('10.0.0.0'))
print(format(a4), format(a4, 's'), format(a4, 'b'), format(a4, '#b'), format(a4, '_b'), format(a4, 'x'),
      format(a4, '#X'), format(a4, '#_x'), format(a4, 'n'), format(a4, '>18s'), format(a4, '*^19s'), f"[{a4:<12s}]")
print(format(IPv6Address('2001:db8::1'), 'n'), format(IPv6Address('::1'), '#_x'))
print(ipaddress.v4_int_to_packed(3221225985), ipaddress.v6_int_to_packed(1), ipaddress.IPV4LENGTH, ipaddress.IPV6LENGTH)

print("== networks")
n4 = ip_network('192.168.0.0/22')
print(repr(n4), n4, n4.network_address, n4.broadcast_address, n4.netmask, n4.hostmask, n4.num_addresses,
      n4.prefixlen, n4.with_prefixlen, n4.with_netmask, n4.with_hostmask, n4.version, n4.max_prefixlen)
print(n4[0], n4[1], n4[-1], n4[-2], IPv4Address('192.168.3.7') in n4, IPv4Address('192.168.4.0') in n4,
      ip_network('192.168.1.0/24').subnet_of(n4), n4.supernet_of(ip_network('192.168.1.0/24')),
      n4.overlaps(ip_network('192.168.3.128/25')), n4.overlaps(ip_network('10.0.0.0/8')))
print(list(n4.subnets()), list(n4.subnets(prefixlen_diff=2)), n4.supernet(), n4.supernet(new_prefix=16),
      n4.supernet(prefixlen_diff=3))
print(list(ip_network('10.0.0.0/29')), list(ip_network('10.0.0.0/29').hosts()))
print(list(ip_network('10.0.0.0/31').hosts()), list(ip_network('10.0.0.7/32').hosts()), list(ip_network('10.0.0.7/32')))
print(IPv4Network('10.0.0.0/255.0.0.0'), IPv4Network('10.0.0.0/0.0.0.255'), IPv4Network(('10.0.0.0', 8)),
      IPv4Network(('10.0.0.0', '255.255.0.0')), IPv4Network('10.1.2.3/8', strict=False), IPv4Network(167772160),
      IPv4Network('0.0.0.0/0').num_addresses)
print(list(ip_network('192.0.2.0/28').address_exclude(ip_network('192.0.2.1/32'))))
print(n4.compare_networks(ip_network('192.168.0.0/23')), n4.compare_networks(n4), ip_network('10.0.0.0/8').compare_networks(n4))
print(n4.is_private, n4.is_global, ip_network('100.64.0.0/10').is_global, ip_network('224.0.0.0/24').is_multicast,
      ip_network('127.0.0.0/8').is_loopback, ip_network('169.254.0.0/16').is_link_local,
      ip_network('240.0.0.0/8').is_reserved, ip_network('0.0.0.0/32').is_unspecified)
n6 = ip_network('2001:db8::/126')
print(repr(n6), n6, n6.exploded, n6.num_addresses, list(n6), list(n6.hosts()), n6.netmask, n6.hostmask,
      n6.broadcast_address, n6.with_netmask, n6.is_private, n6.is_site_local, ip_network('fec0::/12').is_site_local)
print(list(ip_network('2001:db8::/32').subnets(new_prefix=34)), ip_network('2001:db8::/32').supernet(new_prefix=16))
print(ip_network('2001:db8::/32')[2 ** 40], ip_network('2001:db8::/32')[-1], ip_network('fe80::%eth0/64'),
      list(ip_network('::1/128').hosts()), list(ip_network('::/127').hosts()))

print("== interfaces")
i4 = ip_interface('192.0.2.5/24')
i6 = ip_interface('2001:db8::5/64')
print(repr(i4), i4, i4.ip, i4.network, i4.netmask, i4.hostmask, i4.with_prefixlen, i4.with_netmask,
      i4.with_hostmask, i4.exploded, i4 == ip_interface('192.0.2.5/24'), i4 == ip_interface('192.0.2.5/25'),
      i4 < ip_interface('192.0.2.6/24'), i4.is_private, isinstance(i4, IPv4Address), isinstance(i4, ipaddress.IPv4Interface))
print(repr(i6), i6, i6.ip, i6.network, i6.exploded, i6.with_netmask, i6.is_loopback,
      ip_interface('::1/128').is_loopback, ip_interface('::/0').is_unspecified)
print(ipaddress.IPv4Interface(('10.0.0.1', 8)), ipaddress.IPv4Interface(167772161), ipaddress.IPv6Interface('::1'))

print("== functions")
print(list(ipaddress.collapse_addresses([IPv4Network('192.0.2.0/25'), IPv4Network('192.0.2.128/25')])))
print(list(ipaddress.collapse_addresses([IPv4Address('10.0.0.%d' % k) for k in [0, 1, 2, 3, 4, 5, 6, 9, 10]])))
print(list(ipaddress.collapse_addresses([IPv6Network('2001:db8::/33'), IPv6Network('2001:db8:8000::/33'),
                                         IPv6Network('2001:db8::/48')])))
print(list(ipaddress.summarize_address_range(IPv4Address('192.0.2.0'), IPv4Address('192.0.2.130'))))
print(list(ipaddress.summarize_address_range(IPv6Address('2001:db8::'), IPv6Address('2001:db8::1:2'))))
print(list(ipaddress.summarize_address_range(IPv4Address('0.0.0.0'), IPv4Address('255.255.255.255'))))
print(sorted([ip_address('10.0.0.2'), ip_address('10.0.0.1'), ip_address('9.255.0.0')]))
print(sorted([ip_network('10.0.0.0/16'), ip_network('10.0.0.0/8'), ip_network('9.0.0.0/8')]))
print(sorted([ip_address('2001::1'), ip_address('::2'), ip_address('::ffff:0.0.0.1')], key=ipaddress.get_mixed_type_key))
print(ipaddress.get_mixed_type_key(ip_address('1.2.3.4')), ipaddress.get_mixed_type_key(ip_network('1.2.3.0/24')))
seen = {ip_address('10.0.0.1'): 'a', ip_address('::1'): 'b'}
print(seen[ip_address('10.0.0.1')], seen[ip_address('::1')], ip_address('10.0.0.2') in seen,
      len({ip_network('10.0.0.0/8'), ip_network('10.0.0.0/8'), ip_network('10.0.0.0/9')}))
print(isinstance(ip_address('1.2.3.4'), IPv4Address), isinstance(ip_address('::1'), IPv4Address),
      isinstance(ip_network('::/0'), IPv6Network), ip_address('1.2.3.4') == ip_address('::ffff:1.2.3.4'),
      ip_address('1.2.3.4') == ip_network('1.2.3.4/32'))

print("== errors")
def bad_address(s: str) -> None:
    try:
        ip_address(s)
        print("ok", s)
    except ValueError as e:
        print(type(e).__name__, e)

def bad_v4(s: str) -> None:
    try:
        IPv4Address(s)
        print("ok", s)
    except ipaddress.AddressValueError as e:
        print("AddressValueError:", e)

def bad_v6(s: str) -> None:
    try:
        IPv6Address(s)
        print("ok", s)
    except ipaddress.AddressValueError as e:
        print("AddressValueError:", e)

def bad_network(s: str, strict: bool) -> None:
    try:
        print(ip_network(s, strict))
    except ValueError as e:
        print(type(e).__name__, e)

def bad_v4net(s: str) -> None:
    try:
        print(IPv4Network(s))
    except ValueError as e:
        print(type(e).__name__, e)

for s in ['', '1.2.3', '1.2.3.4.5', 'x.y.z.w', '1.2.3.256', '01.2.3.4', '1.2.3.1234', '1..3.4', '1.2.3.4/24', 'hello']:
    bad_address(s)
for s in ['1.2.3', '1.2.3.-4', '1.2.3. 4', '1.2.3.4%x']:
    bad_v4(s)
for s in [':', '1:2', '1::2::3', ':1::2', '1::2:', '1:2:3:4:5:6:7', '1:2:3:4:5:6:7:8:9', '12345::',
          'g::1', '1:2:3:4:5:6:7::8', '::1.2.3.256', 'fe80::1%', 'fe80::1%a%b', '1' * 46, '1:2:3:4:5:6:7:8::']:
    bad_v6(s)
for s in ['1.2.3.4/24', '1.2.3.0/33', '1.2.3.0/-1', '1.2.3.0/255.0.255.0', '1.2.3.0/24/1', '1.2.3.0/x', '1.2.3.0/ 24']:
    bad_network(s, True)
bad_network('1.2.3.4/24', False)
bad_network('2001:db8::1/64', True)
bad_network('2001:db8::/129', True)
bad_v4net('10.0.0.0/255.255.0.0')
bad_v4net('10.0.0.0/0.0.255.255')
bad_v4net('10.0.0.0/255.0.255.255')
try:
    print(IPv4Address('255.255.255.255') + 1)
except ipaddress.AddressValueError as e:
    print("AddressValueError:", e)
try:
    print(IPv4Address('0.0.0.0') - 1)
except ipaddress.AddressValueError as e:
    print("AddressValueError:", e)
try:
    print(IPv6Address('ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff') + 1)
except ipaddress.AddressValueError as e:
    print("AddressValueError:", e)
try:
    IPv4Address(-1)
except ipaddress.AddressValueError as e:
    print("AddressValueError:", e)
try:
    IPv4Address(2 ** 32)
except ipaddress.AddressValueError as e:
    print("AddressValueError:", e)
try:
    IPv4Address(b'\x01\x02')
except ipaddress.AddressValueError as e:
    print("AddressValueError:", e)
try:
    ip_address('1.2.3.4') < ip_address('::1')
except TypeError as e:
    print("TypeError:", e)
try:
    list(ip_network('10.0.0.0/8').subnets(new_prefix=4))
except ValueError as e:
    print("ValueError:", e)
try:
    ip_network('10.0.0.0/8').supernet(new_prefix=9)
except ValueError as e:
    print("ValueError:", e)
try:
    list(ip_network('10.0.0.0/8').address_exclude(ip_network('11.0.0.0/8')))
except ValueError as e:
    print("ValueError:", e)
try:
    ip_network('10.0.0.0/8')[2 ** 24]
except IndexError as e:
    print("IndexError:", e)
try:
    ipaddress.v4_int_to_packed(2 ** 32)
except ValueError as e:
    print("ValueError:", e)
try:
    print(IPv4Address('1.2.3.4').ipv4_mapped)
except AttributeError as e:
    print("AttributeError:", e)
try:
    print(IPv4Address('1.2.3.4').network)
except AttributeError as e:
    print("AttributeError:", e)
