/* ========================= Inflate (RFC 1951) =========================
   After Mark Adler's puff.c: canonical Huffman codes decoded a bit at a time
   (counts per length, symbols in code order). Slower than zlib's tables, but
   small; it runs once per module. See inflate.h. */

#include "inflate.h"
#include <stdlib.h>
#include <string.h>

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    const unsigned char *in; size_t inlen, inpos;
    unsigned char *out; size_t outlen, outpos;
    unsigned bitbuf; int bitcnt;
    int short_input;                                           /* (it ended early: the bits read since are 0) */
} St;

typedef struct { short count[MAXBITS+1]; short symbol[FIXLCODES]; } Huff;

static int bits(St *s, int need){
    unsigned val=s->bitbuf;
    while(s->bitcnt<need){
        if(s->inpos==s->inlen) s->short_input=1;
        else val|=(unsigned)s->in[s->inpos++]<<s->bitcnt;
        s->bitcnt+=8;
    }
    s->bitbuf=val>>need; s->bitcnt-=need;
    return (int)(val&((1u<<need)-1));
}

static int stored(St *s){
    s->bitbuf=0; s->bitcnt=0;                                  /* (to a byte boundary) */
    if(s->inpos+4>s->inlen) return -1;
    unsigned len=s->in[s->inpos]|(unsigned)s->in[s->inpos+1]<<8;
    unsigned nlen=s->in[s->inpos+2]|(unsigned)s->in[s->inpos+3]<<8;
    s->inpos+=4;
    if(len!=(~nlen&0xffff) || s->inpos+len>s->inlen || s->outpos+len>s->outlen) return -1;
    memcpy(s->out+s->outpos,s->in+s->inpos,len); s->inpos+=len; s->outpos+=len;
    return 0;
}

static int decode(St *s, const Huff *h){
    int code=0, first=0, index=0;
    for(int len=1;len<=MAXBITS;len++){
        code|=bits(s,1);
        int count=h->count[len];
        if(code-count<first) return h->symbol[index+(code-first)];
        index+=count; first+=count; first<<=1; code<<=1;
    }
    return -1;
}

/* the decoding tables of a code given by its lengths: 0, or -1 when it is over-subscribed
   (an incomplete code is allowed: a single distance code ...) */
static int build(Huff *h, const short *length, int n){
    short offs[MAXBITS+1];
    for(int len=0;len<=MAXBITS;len++) h->count[len]=0;
    for(int sym=0;sym<n;sym++) h->count[length[sym]]++;
    if(h->count[0]==n) return 0;
    int left=1;
    for(int len=1;len<=MAXBITS;len++){ left<<=1; left-=h->count[len]; if(left<0) return -1; }
    offs[1]=0;
    for(int len=1;len<MAXBITS;len++) offs[len+1]=(short)(offs[len]+h->count[len]);
    for(int sym=0;sym<n;sym++) if(length[sym]) h->symbol[offs[length[sym]]++]=(short)sym;
    return 0;
}

static const short lbase[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short lext[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short dbase[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short dext[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int codes(St *s, const Huff *lencode, const Huff *distcode){
    for(;;){
        int sym=decode(s,lencode);
        if(sym<0 || s->short_input) return -1;
        if(sym<256){
            if(s->outpos==s->outlen) return -1;
            s->out[s->outpos++]=(unsigned char)sym;
        } else if(sym==256) return 0;
        else {
            sym-=257;
            if(sym>=29) return -1;
            size_t len=(size_t)(lbase[sym]+bits(s,lext[sym]));
            int dsym=decode(s,distcode);
            if(dsym<0 || dsym>=30) return -1;
            size_t dist=(size_t)(dbase[dsym]+bits(s,dext[dsym]));
            if(dist>s->outpos || s->outpos+len>s->outlen) return -1;
            unsigned char *o=s->out+s->outpos;
            for(size_t i=0;i<len;i++) o[i]=o[(ptrdiff_t)i-(ptrdiff_t)dist];   /* (may overlap: a run) */
            s->outpos+=len;
        }
    }
}

static int fixed(St *s){
    static Huff lencode, distcode; static int ready;
    if(!ready){
        short lengths[FIXLCODES]; int sym=0;
        for(;sym<144;sym++) lengths[sym]=8;
        for(;sym<256;sym++) lengths[sym]=9;
        for(;sym<280;sym++) lengths[sym]=7;
        for(;sym<FIXLCODES;sym++) lengths[sym]=8;
        build(&lencode,lengths,FIXLCODES);
        for(sym=0;sym<MAXDCODES;sym++) lengths[sym]=5;
        build(&distcode,lengths,MAXDCODES);
        ready=1;
    }
    return codes(s,&lencode,&distcode);
}

static int dynamic(St *s){
    static const short order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[MAXLCODES+MAXDCODES];
    Huff lencode, distcode;
    int nlen=bits(s,5)+257, ndist=bits(s,5)+1, ncode=bits(s,4)+4;
    if(nlen>MAXLCODES || ndist>MAXDCODES) return -1;
    int index=0;
    for(;index<ncode;index++) lengths[order[index]]=(short)bits(s,3);
    for(;index<19;index++) lengths[order[index]]=0;
    if(build(&lencode,lengths,19)) return -1;
    index=0;
    while(index<nlen+ndist){
        int sym=decode(s,&lencode);
        if(sym<0) return -1;
        if(sym<16) lengths[index++]=(short)sym;
        else {
            int len=0, rep;
            if(sym==16){ if(index==0) return -1; len=lengths[index-1]; rep=3+bits(s,2); }
            else if(sym==17) rep=3+bits(s,3);
            else rep=11+bits(s,7);
            if(index+rep>nlen+ndist) return -1;
            while(rep--) lengths[index++]=(short)len;
        }
    }
    if(lengths[256]==0) return -1;
    if(build(&lencode,lengths,nlen)) return -1;
    if(build(&distcode,lengths+nlen,ndist)) return -1;
    return codes(s,&lencode,&distcode);
}

int mpy_inflate(const unsigned char *in, size_t inlen, unsigned char *out, size_t outlen){
    St s; memset(&s,0,sizeof s);
    s.in=in; s.inlen=inlen; s.out=out; s.outlen=outlen;
    int last;
    do{
        last=bits(&s,1);
        int type=bits(&s,2), err;
        if(type==0) err=stored(&s);
        else if(type==1) err=fixed(&s);
        else if(type==2) err=dynamic(&s);
        else err=-1;
        if(err || s.short_input) return -1;
    } while(!last);
    return s.outpos==outlen ? 0 : -1;
}

const char *mpy_unpack(MpyPacked *p){
    if(!p->text){
        char *t=(char*)malloc((size_t)p->len+1);
        if(!t) return NULL;
        if(mpy_inflate(p->data,p->clen,(unsigned char*)t,p->len)){ free(t); return NULL; }
        t[p->len]=0;
        p->text=t;
    }
    return p->text;
}
