/* pack: what plainpy carries, packed (a build tool).

     pack lib OUT.lib ID.h ROOT [-i MOD]... [-c MOD]... [-t NAME FILE]... FILE.py...
         plainpy.lib, the one file of data both programs read: the Python
         modules of src/stdlib (FILE under ROOT: ROOT/a/b.py is a.b, a/__init__.py
         the package a) without comments and docstrings (the lines stay where they
         were), and texts (-t: the runtime routines of compiled programs ...), each
         deflated on its own. -i MOD: MOD and its submodules only for plainpy (the
         interpreter), -c MOD: only for plainpyc (the compiler). ID.h: the id the
         programs check the library by.
     pack kpack IN OUT
         a KolibriOS executable packed as kpack packs one (the kernel unpacks it).

   plainpy inflates a module when it is first imported (src/inflate.c). Every
   stream is inflated again here and compared before it is written. A docstring
   becomes `pass` (a body may have nothing else), a module's docstring nothing;
   one with code after it on its line stays. Built with the host compiler
   (cross builds too) from src/py_lex.c, src/util.c, src/inflate.c and
   src/aot/kpack.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "py_lex.h"
#include "inflate.h"
#include "util.h"

int aot_kpack(const unsigned char *in, size_t n, unsigned char **out, size_t *outn);   /* src/aot/kpack.c */

/* ---------------------------------------------------------------- deflate */

typedef struct { unsigned char *b; size_t n, cap; unsigned bits; int nbits; } Out;

static void out_byte(Out *o, int c){
    if(o->n==o->cap){ o->cap=o->cap?o->cap*2:4096; o->b=(unsigned char*)xrealloc(o->b,o->cap); }
    o->b[o->n++]=(unsigned char)c;
}
static void put(Out *o, unsigned v, int n){                     /* n bits of v, LSB first */
    o->bits|=v<<o->nbits; o->nbits+=n;
    while(o->nbits>=8){ out_byte(o,(int)(o->bits&255)); o->bits>>=8; o->nbits-=8; }
}
static void flush_bits(Out *o){ if(o->nbits) out_byte(o,(int)(o->bits&255)); o->bits=0; o->nbits=0; }
static unsigned rev(unsigned code, int len){ unsigned r=0; while(len--){ r=(r<<1)|(code&1); code>>=1; } return r; }

static const int lbase[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const int lext[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const int dbase[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const int dext[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int len_code(int len){ int c=28; while(lbase[c]>len) c--; return c; }
static int dist_code(int dist){ int c=29; while(dbase[c]>dist) c--; return c; }

/* code lengths of a Huffman code for freq[0..n), none longer than maxlen
   (frequencies halved until the tree is shallow enough) */
static void huff_lengths(const unsigned *freq0, int n, int maxlen, unsigned char *len){
    unsigned freq[320]; int parent[640]; unsigned w[640]; int alive[640];
    for(int i=0;i<n;i++) freq[i]=freq0[i];
    for(;;){
        int nn=0, leaves[320], nl=0;
        for(int i=0;i<n;i++){ len[i]=0; if(freq[i]){ leaves[nl++]=i; } }
        if(nl==0) return;
        if(nl==1){ len[leaves[0]]=1; return; }
        for(int k=0;k<nl;k++){ w[k]=freq[leaves[k]]; alive[k]=1; parent[k]=-1; }
        nn=nl;
        for(int left=nl;left>1;left--){                        /* (n is small: the two lightest by a scan) */
            int a=-1, b=-1;
            for(int k=0;k<nn;k++) if(alive[k]){
                if(a<0 || w[k]<w[a]){ b=a; a=k; } else if(b<0 || w[k]<w[b]) b=k; }
            alive[a]=alive[b]=0; w[nn]=w[a]+w[b]; alive[nn]=1; parent[nn]=-1; parent[a]=parent[b]=nn; nn++;
        }
        int deepest=0;
        for(int k=0;k<nl;k++){ int d=0; for(int p=parent[k];p>=0;p=parent[p]) d++; len[leaves[k]]=(unsigned char)d; if(d>deepest) deepest=d; }
        if(deepest<=maxlen) return;
        for(int i=0;i<n;i++) if(freq[i]) freq[i]=(freq[i]+1)/2;
    }
}
static void canonical(const unsigned char *len, int n, unsigned *code){
    int count[16]={0}; unsigned next[16];
    for(int i=0;i<n;i++) count[len[i]]++;
    count[0]=0; unsigned c=0;
    for(int b=1;b<16;b++){ c=(c+(unsigned)count[b-1])<<1; next[b]=c; }
    for(int i=0;i<n;i++) code[i]= len[i] ? rev(next[len[i]]++,len[i]) : 0;
}

typedef struct { int lit; int len, dist; } Tok;                /* lit >= 0, or a match */

#define WBITS 15
#define WSIZE (1<<WBITS)
#define HSIZE (1<<15)
#define MAXCHAIN 2048

static int hash3(const unsigned char *s){ return ((s[0]<<10)^(s[1]<<5)^s[2])&(HSIZE-1); }

/* the longest earlier match at pos (length 0 when it is shorter than 3) */
static int longest(const unsigned char *s, int n, int pos, const int *head, const int *prev, int *dist){
    if(pos+3>n) return 0;
    int best=0, limit=n-pos<258?n-pos:258, chain=MAXCHAIN;
    for(int c=head[hash3(s+pos)]; c>=0 && c<pos && pos-c<=WSIZE-1 && chain--; ){
        if(s[c+best]==s[pos+best] && s[c]==s[pos]){
            int l=0; while(l<limit && s[c+l]==s[pos+l]) l++;
            if(l>best){ best=l; *dist=pos-c; if(l==limit) break; }
        }
        int nx=prev[c&(WSIZE-1)];
        if(nx>=c) break;                                         /* (a slot reused by a later position: the chain ends) */
        c=nx;
    }
    return best>=3 ? best : 0;
}

static unsigned char *deflate(const unsigned char *s, int n, size_t *outlen){
    int *head=(int*)xmalloc(sizeof(int)*HSIZE), *prev=(int*)xmalloc(sizeof(int)*WSIZE);
    for(int i=0;i<HSIZE;i++) head[i]=-1;
    Tok *t=(Tok*)xmalloc(sizeof(Tok)*(size_t)(n+1)); int nt=0;
    int pos=0;
    #define INSERT(p) do{ if((p)+3<=n){ int h_=hash3(s+(p)); prev[(p)&(WSIZE-1)]=head[h_]; head[h_]=(p); } }while(0)
    while(pos<n){
        int d1=0, l1=longest(s,n,pos,head,prev,&d1);
        if(l1){                                                  /* a longer one at the next byte: a literal first (lazy) */
            INSERT(pos);
            int d2=0, l2= l1<32 ? longest(s,n,pos+1,head,prev,&d2) : 0;
            if(l2>l1){ t[nt].lit=s[pos]; nt++; pos++; continue; }
            t[nt].lit=-1; t[nt].len=l1; t[nt].dist=d1; nt++;
            for(int k=1;k<l1;k++) INSERT(pos+k);
            pos+=l1;
        } else { INSERT(pos); t[nt].lit=s[pos]; nt++; pos++; }
    }
    unsigned lf[286]={0}, df[30]={0};
    for(int i=0;i<nt;i++){ if(t[i].lit>=0) lf[t[i].lit]++; else { lf[257+len_code(t[i].len)]++; df[dist_code(t[i].dist)]++; } }
    lf[256]=1;
    { int used=0; for(int i=0;i<286;i++) used+=lf[i]>0; if(used<2) lf[0]++; }   /* (two codes at least, for every decoder) */
    { int used=0; for(int i=0;i<30;i++) used+=df[i]>0; if(used<2){ if(!df[0]) df[0]=1; else df[1]=1; } }
    unsigned char ll[286], dl[30]; unsigned lc[286], dc[30];
    huff_lengths(lf,286,15,ll); huff_lengths(df,30,15,dl);
    canonical(ll,286,lc); canonical(dl,30,dc);
    int nlit=286; while(nlit>257 && !ll[nlit-1]) nlit--;
    int ndist=30; while(ndist>1 && !dl[ndist-1]) ndist--;
    /* the code lengths, run-length coded (16: the last again, 17/18: zeros) */
    unsigned char all[316]; int na=0;
    for(int i=0;i<nlit;i++) all[na++]=ll[i];
    for(int i=0;i<ndist;i++) all[na++]=dl[i];
    int rs[316], rx[316], nr=0;                                 /* symbol, extra bits value */
    for(int i=0;i<na;){
        int v=all[i], r=1; while(i+r<na && all[i+r]==v) r++;
        i+=r;
        if(v==0){
            while(r>=11){ int m=r<138?r:138; rs[nr]=18; rx[nr++]=m-11; r-=m; }
            if(r>=3){ rs[nr]=17; rx[nr++]=r-3; r=0; }
            while(r--){ rs[nr]=0; rx[nr++]=0; }
        } else {
            rs[nr]=v; rx[nr++]=0; r--;
            while(r>=3){ int m=r<6?r:6; rs[nr]=16; rx[nr++]=m-3; r-=m; }
            while(r--){ rs[nr]=v; rx[nr++]=0; }
        }
    }
    unsigned cf[19]={0}; for(int i=0;i<nr;i++) cf[rs[i]]++;
    unsigned char cl[19]; unsigned cc[19];
    huff_lengths(cf,19,7,cl); canonical(cl,19,cc);
    static const int order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int ncl=19; while(ncl>4 && !cl[order[ncl-1]]) ncl--;
    Out o; memset(&o,0,sizeof o);
    put(&o,1,1); put(&o,2,2);                                    /* the last block, dynamic codes */
    put(&o,(unsigned)(nlit-257),5); put(&o,(unsigned)(ndist-1),5); put(&o,(unsigned)(ncl-4),4);
    for(int i=0;i<ncl;i++) put(&o,cl[order[i]],3);
    for(int i=0;i<nr;i++){
        put(&o,cc[rs[i]],cl[rs[i]]);
        if(rs[i]==16) put(&o,(unsigned)rx[i],2); else if(rs[i]==17) put(&o,(unsigned)rx[i],3); else if(rs[i]==18) put(&o,(unsigned)rx[i],7);
    }
    for(int i=0;i<nt;i++){
        if(t[i].lit>=0){ put(&o,lc[t[i].lit],ll[t[i].lit]); continue; }
        int c=len_code(t[i].len), d=dist_code(t[i].dist);
        put(&o,lc[257+c],ll[257+c]); if(lext[c]) put(&o,(unsigned)(t[i].len-lbase[c]),lext[c]);
        put(&o,dc[d],dl[d]); if(dext[d]) put(&o,(unsigned)(t[i].dist-dbase[d]),dext[d]);
    }
    put(&o,lc[256],ll[256]);
    flush_bits(&o);
    free(head); free(prev); free(t);
    *outlen=o.n;
    return o.b;
}

/* ---------------------------------------------------------------- Python: comments and docstrings */

static int docstring_prefix_ok(const char *s, int a){             /* a plain (or r/u) string: f, b and t ones are no docstrings */
    for(int i=a; s[i]!='\'' && s[i]!='"'; i++) if(strchr("fFbBtT",s[i])) return 0;
    return 1;
}

typedef struct { int start, end, module; } Doc;

/* src without comments and docstrings, as many lines as it had; NULL when it does not tokenize */
static char *strip_python(const char *path, const char *src, int len, int *outlen){
    PyLexer lx; memset(&lx,0,sizeof lx);
    int start= len>=3 && (unsigned char)src[0]==0xEF && (unsigned char)src[1]==0xBB && (unsigned char)src[2]==0xBF ? 3 : 0;
    if(py_lex(&lx,src,len,start,0,NULL)){ fprintf(stderr,"pack: %s:%d: %s (left as it is)\n",path,lx.error_line,lx.error); return NULL; }
    PyTok *t=lx.v; int n=lx.n;
    Doc *docs=(Doc*)xmalloc(sizeof(Doc)*(size_t)(n+1)); int nd=0;
    int *docpos=(int*)xmalloc(sizeof(int)*(size_t)(n+1)); int np=0;   /* token indexes where a docstring may be */
    { int i=0; while(i<n && (t[i].kind==PT_NEWLINE||t[i].kind==PT_INDENT||t[i].kind==PT_DEDENT)) i++; docpos[np++]=i; }
    for(int i=0;i<n;i++){
        int stmt_start= i==0 || t[i-1].kind==PT_NEWLINE || t[i-1].kind==PT_INDENT || t[i-1].kind==PT_DEDENT;
        if(!stmt_start || t[i].kind!=PT_NAME) continue;
        int k=i;
        if(t[k].op==KW_async && k+1<n && t[k+1].kind==PT_NAME && t[k+1].op==KW_def) k++;
        if(t[k].op!=KW_def && t[k].op!=KW_class) continue;
        int depth=0, j=k+1;
        for(;j<n && t[j].kind!=PT_NEWLINE && t[j].kind!=PT_END;j++){
            if(t[j].kind!=PT_OP) continue;
            int op=t[j].op;
            if(op==O_LPAR||op==O_LSQB||op==O_LBRACE) depth++;
            else if(op==O_RPAR||op==O_RSQB||op==O_RBRACE) depth--;
            else if(op==O_COLON && depth==0) break;
        }
        if(j>=n || t[j].kind!=PT_OP || t[j].op!=O_COLON) continue;
        j++;
        if(j<n && t[j].kind==PT_NEWLINE){ j++; if(j<n && t[j].kind==PT_INDENT) j++; else continue; }
        docpos[np++]=j;
    }
    for(int p=0;p<np;p++){
        int a=docpos[p], b=a;
        while(b<n && t[b].kind==PT_STRING && docstring_prefix_ok(src,t[b].start)) b++;
        if(b==a || b>=n || (t[b].kind!=PT_NEWLINE && t[b].kind!=PT_END && !(t[b].kind==PT_OP && t[b].op==O_SEMI))) continue;
        if(b<n && t[b].kind==PT_STRING) continue;                /* (an f-string after it: one expression, not a docstring) */
        int e=t[b-1].end, q=e;
        while(q<len && (src[q]==' '||src[q]=='\t')) q++;
        if(q<len && src[q]!='\n' && src[q]!='\r' && src[q]!='#') continue;   /* (code after it on its line: it stays) */
        docs[nd].start=t[a].start; docs[nd].end=e; docs[nd].module=(p==0); nd++;
    }
    /* strings (where a '#' is no comment) in order of their start */
    char *out=(char*)xmalloc((size_t)len+16); int o=0;
    int ti=0, di=0;
    for(int i=0;i<len;){
        while(ti<n && t[ti].end<=i) ti++;
        if(di<nd && docs[di].start==i){                          /* a docstring: `pass` and its newlines */
            if(!docs[di].module){ memcpy(out+o,"pass",4); o+=4; }
            for(int k=docs[di].start;k<docs[di].end;k++) if(src[k]=='\n') out[o++]='\n';
            i=docs[di].end; di++; continue;
        }
        if(ti<n && t[ti].kind==PT_STRING && t[ti].start<=i && i<t[ti].end){   /* inside a string: as it is */
            int e=t[ti].end; memcpy(out+o,src+i,(size_t)(e-i)); o+=e-i; i=e; continue;
        }
        if(src[i]=='#'){                                         /* a comment: gone, and the blanks before it */
            while(o>0 && (out[o-1]==' '||out[o-1]=='\t')) o--;
            while(i<len && src[i]!='\n' && src[i]!='\r') i++;
            continue;
        }
        if(src[i]=='\n'){                                        /* (blanks at the end of a line: gone) */
            while(o>0 && (out[o-1]==' '||out[o-1]=='\t')) o--;
        }
        out[o++]=src[i++];
    }
    free(docs); free(docpos); free(lx.v); free(lx.line_starts);
    *outlen=o;
    return out;
}

/* ---------------------------------------------------------------- output */

static char *read_file(const char *path, int *len){
    FILE *f=fopen(path,"rb"); if(!f){ fprintf(stderr,"pack: cannot read %s\n",path); exit(1); }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char *b=(char*)xmalloc((size_t)n+1);
    if(n && fread(b,1,(size_t)n,f)!=(size_t)n){ fprintf(stderr,"pack: cannot read %s\n",path); exit(1); }
    fclose(f); b[n]=0; *len=(int)n; return b;
}

/* deflated, and checked by inflating it again */
static unsigned char *pack_checked(const char *what, const char *s, int n, size_t *clen){
    unsigned char *c=deflate((const unsigned char*)s,n,clen);
    unsigned char *back=(unsigned char*)xmalloc((size_t)n+1);
    if(ppy_inflate(c,*clen,back,(size_t)n) || memcmp(back,s,(size_t)n)){ fprintf(stderr,"pack: %s: inflating it again does not give it back\n",what); exit(1); }
    free(back);
    return c;
}

/* ---------------------------------------------------------------- plainpy.lib */

typedef struct { char *name; int kind, who; unsigned char *z; size_t clen, len; } Entry;   /* kind: LIB_MODULE ... */
enum { LIB_MODULE=0, LIB_PACKAGE=1, LIB_TEXT=2 };
enum { FOR_INTERP=1, FOR_COMPILER=2 };

static int names_module(const char *name, int n, char **mods){   /* name is one of mods or a submodule of one */
    for(int i=0;i<n;i++){ size_t l=strlen(mods[i]); if(!strncmp(name,mods[i],l) && (name[l]==0 || name[l]=='.')) return 1; }
    return 0;
}

static unsigned fnv(unsigned h, const unsigned char *b, size_t n){ for(size_t i=0;i<n;i++){ h^=b[i]; h*=16777619u; } return h; }
static void put32(FILE *f, unsigned v){ fputc((int)(v&255),f); fputc((int)(v>>8&255),f); fputc((int)(v>>16&255),f); fputc((int)(v>>24&255),f); }
static void le32(unsigned char *b, unsigned v){ b[0]=(unsigned char)v; b[1]=(unsigned char)(v>>8); b[2]=(unsigned char)(v>>16); b[3]=(unsigned char)(v>>24); }

/* pack lib OUT.lib ID.h ROOT [-i MOD]... [-c MOD]... [-t NAME FILE]... FILE.py...
   The file (little-endian; see src/pystdlib.c, which reads it):
       "PPYLIB\0\1", u32 id, u32 entries, u32 index bytes,
       per entry: u8 kind (0 module, 1 package, 2 text), u8 for (1 plainpy, 2 plainpyc, 3 both),
                  u16 name length, the name, u32 offset in the data, u32 deflated length, u32 length
       the data: each entry deflated on its own
   id: FNV-1a of index and data; ID.h has it as PPY_LIB_ID, so the programs
   take only the library of their own build. */
static int pack_lib(const char *outpath, const char *idpath, const char *root, int argc, char **argv){
    char **imods=(char**)xmalloc(sizeof(char*)*(size_t)(argc+1)), **cmods=(char**)xmalloc(sizeof(char*)*(size_t)(argc+1));
    int ni=0, nc=0;
    Entry *e=(Entry*)xmalloc(sizeof(Entry)*(size_t)(argc+1)); int ne=0;
    size_t raw=0, stripped=0, total=0;
    size_t rl=strlen(root); while(rl && root[rl-1]=='/') rl--;
    int i=0;
    for(;i<argc;i++){
        if(!strcmp(argv[i],"-i") && i+1<argc){ imods[ni++]=argv[++i]; continue; }
        if(!strcmp(argv[i],"-c") && i+1<argc){ cmods[nc++]=argv[++i]; continue; }
        if(!strcmp(argv[i],"-t") && i+2<argc){
            Entry *t=&e[ne++]; t->name=argv[i+1]; t->kind=LIB_TEXT; t->who=FOR_COMPILER;
            int len; char *s=read_file(argv[i+2],&len);
            t->z=pack_checked(argv[i+2],s,len,&t->clen); t->len=(size_t)len; free(s);
            fprintf(stderr,"pack: %s: %d bytes -> %zu deflated\n",t->name,len,t->clen);
            total+=t->clen; i+=2; continue; }
        break;
    }
    for(;i<argc;i++){
        const char *p=argv[i];
        if(strncmp(p,root,rl) || p[rl]!='/'){ fprintf(stderr,"pack: %s is not under %s\n",p,root); return 1; }
        char *name=xstrdup2(p+rl+1); size_t nl=strlen(name);
        if(nl<3 || strcmp(name+nl-3,".py")){ fprintf(stderr,"pack: %s is no .py file\n",p); return 1; }
        name[nl-3]=0;
        Entry *m=&e[ne++]; m->kind=LIB_MODULE;
        size_t k=strlen(name);
        if(k>=9 && !strcmp(name+k-9,"/__init__")){ name[k-9]=0; m->kind=LIB_PACKAGE; }
        else if(!strcmp(name,"__init__")){ fprintf(stderr,"pack: %s: no package name\n",p); return 1; }
        for(char *q=name;*q;q++) if(*q=='/') *q='.';
        m->name=name;
        m->who= names_module(name,ni,imods) ? FOR_INTERP : names_module(name,nc,cmods) ? FOR_COMPILER : FOR_INTERP|FOR_COMPILER;
        int len; char *src=read_file(p,&len);
        int sl; char *s=strip_python(p,src,len,&sl);
        if(!s){ s=src; sl=len; src=NULL; }
        m->z=pack_checked(p,s,sl,&m->clen); m->len=(size_t)sl;
        raw+=(size_t)len; stripped+=(size_t)sl; total+=m->clen;
        free(s); free(src);
    }
    size_t isize=0;
    for(int k=0;k<ne;k++) isize+=16+strlen(e[k].name);
    unsigned char *index=(unsigned char*)xmalloc(isize+1), *q=index; unsigned off=0;
    for(int k=0;k<ne;k++){
        size_t l=strlen(e[k].name);
        q[0]=(unsigned char)e[k].kind; q[1]=(unsigned char)e[k].who; q[2]=(unsigned char)l; q[3]=(unsigned char)(l>>8);
        memcpy(q+4,e[k].name,l); q+=4+l;
        le32(q,off); le32(q+4,(unsigned)e[k].clen); le32(q+8,(unsigned)e[k].len); q+=12;
        off+=(unsigned)e[k].clen;
    }
    unsigned id=fnv(2166136261u,index,isize);
    for(int k=0;k<ne;k++) id=fnv(id,e[k].z,e[k].clen);
    FILE *f=fopen(outpath,"wb"); if(!f){ fprintf(stderr,"pack: cannot write %s\n",outpath); return 1; }
    fwrite("PPYLIB\0\1",1,8,f); put32(f,id); put32(f,(unsigned)ne); put32(f,(unsigned)isize);
    fwrite(index,1,isize,f);
    for(int k=0;k<ne;k++) fwrite(e[k].z,1,e[k].clen,f);
    if(fclose(f)){ fprintf(stderr,"pack: cannot write %s\n",outpath); return 1; }
    f=fopen(idpath,"w"); if(!f){ fprintf(stderr,"pack: cannot write %s\n",idpath); return 1; }
    fprintf(f,"/* generated by tools/pack.c: the id of the plainpy.lib of this build */\n#define PPY_LIB_ID 0x%08xu\n",id);
    fclose(f);
    fprintf(stderr,"pack: %s: %d entries, the modules %zu bytes -> %zu without comments and docstrings; %zu bytes in all\n",
            outpath,ne,raw,stripped,20+isize+total);
    return 0;
}

/* ---------------------------------------------------------------- kpack */

/* pack kpack IN OUT: a KolibriOS executable packed as the system's kpack packs
   one (src/aot/kpack.c, what plainpyc does to the programs it compiles); the
   kernel unpacks it while loading it */
static int pack_kpack(const char *in, const char *out){
    int n; char *raw=read_file(in,&n);
    unsigned char *packed; size_t pn;
    if(aot_kpack((const unsigned char*)raw,(size_t)n,&packed,&pn)){ packed=(unsigned char*)raw; pn=(size_t)n; raw=NULL; }
    FILE *f=fopen(out,"wb"); if(!f){ fprintf(stderr,"pack: cannot write %s\n",out); return 1; }
    fwrite(packed,1,pn,f);
    if(fclose(f)){ fprintf(stderr,"pack: cannot write %s\n",out); return 1; }
    fprintf(stderr,"pack: %s: %d bytes -> %zu kpacked\n",out,n,pn);
    free(raw);
    return 0;
}

int main(int argc, char **argv){
    if(argc>=5 && !strcmp(argv[1],"lib")) return pack_lib(argv[2],argv[3],argv[4],argc-5,argv+5);
    if(argc==4 && !strcmp(argv[1],"kpack")) return pack_kpack(argv[2],argv[3]);
    fprintf(stderr,"usage: pack lib OUT.lib ID.h ROOT [-i MOD]... [-c MOD]... [-t NAME FILE]... FILE.py...\n"
                   "       pack kpack IN OUT\n");
    return 2;
}
