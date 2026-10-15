# mimetypes. The guesses go through a MimeTypes of the built-in tables only:
# the module's own functions add the system's mime.types (/etc/mime.types,
# /etc/apache2/mime.types ...), which differ from one system to another.
import mimetypes
db = mimetypes.MimeTypes()
for f in ['a.html', 'b.txt', 'c.png', 'd.json', 'e.tar.gz', 'f.unknownext', 'g.JPG', 'http://x/y.css?q=1', 'h.svg',
          'i.js', 'j.mp3', 'k.pdf', 'l.zip', 'm.py', 'n.c', 'o.wasm', 'p.csv', 'q.xml', 'r.gif', 's.webp', 't.ico',
          'u.woff2', 'v.md', 'data:text/plain;base64,xx', 'w.tgz']:
    print(f, db.guess_type(f))
print(db.guess_extension('text/html'), db.guess_extension('image/jpeg'), db.guess_extension('application/json'), sorted(db.guess_all_extensions('image/jpeg')))
print(db.guess_file_type('x.tgz'), db.guess_type('a.rtf', strict=False), db.guess_type('a.rtf'))
mimetypes.add_type('application/x-mine', '.mine')
print(mimetypes.guess_type('z.mine'), mimetypes.guess_extension('application/x-mine'), mimetypes.types_map['.png'], mimetypes.encodings_map['.gz'])
