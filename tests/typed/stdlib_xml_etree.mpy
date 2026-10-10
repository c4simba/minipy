# xml.etree.ElementTree: parsing (attributes, text / tail, entities, CDATA, comments, namespaces), the tree
# API, ElementPath (find / findall / findtext / iter), building, tostring (encodings, escaping), indent,
# write / parse with files, errors
import xml.etree.ElementTree as ET

doc = """<?xml version="1.0"?>
<!-- the catalog -->
<catalog xmlns:dc="http://purl.org/dc/elements/1.1/">
  <book id="b1" lang="en">
    <title>Python &amp; XML</title>
    <dc:creator>Ann</dc:creator>
    <price currency="EUR">29.90</price>
    <tags><tag>xml</tag><tag>python</tag></tags>
  </book>
  <book id="b2">
    <title><![CDATA[<Fun> & games]]></title>
    <price currency="USD">9.50</price>
    <note>caf&#233; &#x263A;</note>
  </book>
  <!-- end -->
</catalog>"""

root = ET.fromstring(doc)
print(root.tag, len(root), root.attrib, repr(root.text))
for book in root.findall("book"):
    print(book.get("id"), book.get("lang"), book.get("lang", "none"), book.findtext("title"), book.find("price").get("currency"))
print([t.text for t in root.iter("tag")])
print(root.find("book/{http://purl.org/dc/elements/1.1/}creator").text)
print(root.findtext("book/dc:creator", namespaces={"dc": "http://purl.org/dc/elements/1.1/"}))
print([b.get("id") for b in root.findall(".//price/..")], len(root.findall(".//tag")))
print([b.get("id") for b in root.findall("book[@lang]")], [b.get("id") for b in root.findall("book[@id='b2']")])
print([p.text for p in root.findall("book/price")], root.findtext("book[2]/note"), root.findtext("nothing", "dflt"))
print([b.get("id") for b in root.findall("book[note]")], [t.text for t in root.findall(".//tags/tag[last()]")])
print(repr("".join(root.find("book").itertext()).split()[:4]))
print(root[0][0].tag, repr(root[0][0].tail), root[1].keys(), root[1].items())

# building
top = ET.Element("top", {"v": "1"})
c1 = ET.SubElement(top, "child", name="a")
c1.text = "x < y & \"z\""
c2 = ET.SubElement(top, "child", attrib={"name": "b"})
c2.set("extra", "line\nbreak\t\"q\"")
ET.SubElement(c2, "empty")
c2.tail = "after"
top.append(ET.Comment(" note "))
print(ET.tostring(top, encoding="unicode"))
print(ET.tostring(top))
print(ET.tostring(top, short_empty_elements=False, encoding="unicode"))
uni = ET.Element("u")
uni.text = "café"
print(ET.tostring(uni), ET.tostring(uni, encoding="utf-8"), ET.tostring(uni, encoding="unicode"))
print(ET.tostring(uni, encoding="iso-8859-1"))
print(ET.tostring(top, method="text", encoding="unicode"))
top.remove(c1)
top.insert(0, ET.Element("first"))
print([e.tag for e in top if e.text != " note "], len(top), ET.iselement(top), ET.iselement("x"))

ET.indent(top)
print(ET.tostring(top, encoding="unicode"))

ns = ET.Element("{urn:x}root")
ET.SubElement(ns, "{urn:x}item").text = "1"
ET.SubElement(ns, "{http://purl.org/dc/elements/1.1/}title").text = "t"
print(ET.tostring(ns, encoding="unicode"))
ET.register_namespace("x", "urn:x")
print(ET.tostring(ns, encoding="unicode"))

# files
tree = ET.ElementTree(top)
tree.write("build/typed_etree.xml")
again = ET.parse("build/typed_etree.xml")
print(again.getroot().tag, [e.tag for e in again.getroot()], again.findtext("child"))
tree.write("build/typed_etree.xml", encoding="utf-8", xml_declaration=True)
with open("build/typed_etree.xml", "rb") as f:
    print(f.read()[:40])

for bad in ("<a><b></a>", "<a>", "", "<a></a><b/>", "<a x='1' x='2'/>", "<a>&nope;</a>", "text", "<a b=1/>"):
    try:
        ET.fromstring(bad)
        print("parsed", repr(bad))
    except ET.ParseError as e:
        print("ParseError:", e, e.position)
