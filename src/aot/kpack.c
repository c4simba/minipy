/* ========================= Typed compiler: kpack =========================
   A KolibriOS program packed the way the system's own kpack packs one, so
   the kernel unpacks it as it loads it (core/dll.inc: load_file,
   unpacker.inc: unpack):

       'KPCK', dd the unpacked size, dd the method (1 LZMA, +0x40 calltrick)
       the LZMA stream: lc=3 lp=0 pb=2, no header; the range coder's first
       byte (always 0) left out and its next four as one little-endian dword
       calltrick: dd how many calls / jumps, db their marker

   calltrick: before compressing, the rel32 of every call / jmp (E8 / E9)
   whose target is inside the program becomes its absolute address, written
   big-endian with the marker - a byte no unconverted E8 / E9 is followed by -
   in place of its high byte (0): the same target looks the same everywhere,
   which LZMA likes. The unpacker turns them back.

   The LZMA encoder here is a small one - hash chains, lazy matching, the
   last four distances tried at every position - with no price model; the
   stream is plain LZMA, any decoder reads it. */

#include "aot.h"
#include "util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- range encoder */

typedef uint16_t Prob;
enum { kBits=11, kTotal=1<<kBits, kMove=5, kTop=1<<24 };

typedef struct {
    uint64_t low; uint32_t range; uint8_t cache; uint64_t cache_size;
    uint8_t *out; size_t n, cap;
} RC;

static void rc_byte(RC *rc, uint8_t b){
    if(rc->n==rc->cap){ rc->cap=rc->cap?rc->cap*2:65536; rc->out=(uint8_t*)xrealloc(rc->out,rc->cap); }
    rc->out[rc->n++]=b;
}
static void rc_shift_low(RC *rc){
    if((uint32_t)rc->low<0xFF000000u || (rc->low>>32)!=0){
        uint8_t carry=(uint8_t)(rc->low>>32), t=rc->cache;
        do{ rc_byte(rc,(uint8_t)(t+carry)); t=0xFF; } while(--rc->cache_size);
        rc->cache=(uint8_t)((uint32_t)rc->low>>24);
    }
    rc->cache_size++;
    rc->low=(uint32_t)rc->low<<8;
}
static void rc_bit(RC *rc, Prob *p, unsigned bit){
    uint32_t bound=(rc->range>>kBits)*(*p);
    if(!bit){ rc->range=bound; *p=(Prob)(*p+((kTotal-*p)>>kMove)); }
    else { rc->low+=bound; rc->range-=bound; *p=(Prob)(*p-(*p>>kMove)); }
    while(rc->range<kTop){ rc->range<<=8; rc_shift_low(rc); }
}
static void rc_direct(RC *rc, uint32_t v, int nbits){
    while(nbits--){
        rc->range>>=1;
        if((v>>nbits)&1) rc->low+=rc->range;
        while(rc->range<kTop){ rc->range<<=8; rc_shift_low(rc); }
    }
}
static void rc_flush(RC *rc){ for(int i=0;i<5;i++) rc_shift_low(rc); }
static void tree(RC *rc, Prob *p, int nbits, uint32_t sym){
    uint32_t m=1;
    for(int i=nbits-1;i>=0;i--){ unsigned b=(sym>>i)&1; rc_bit(rc,&p[m],b); m=(m<<1)|b; }
}
static void tree_rev(RC *rc, Prob *p, int nbits, uint32_t sym){
    uint32_t m=1;
    for(int i=0;i<nbits;i++){ unsigned b=sym&1; sym>>=1; rc_bit(rc,&p[m],b); m=(m<<1)|b; }
}

/* ---------------------------------------------------------------- the LZMA model */

enum { LC=3, PB=2, NSTATES=12, POSSTATES=1<<PB, MINLEN=2, MAXLEN=273 };

typedef struct { Prob choice, choice2, low[POSSTATES][8], mid[POSSTATES][8], high[256]; } LenEnc;
typedef struct {
    RC rc;
    Prob is_match[NSTATES][16], is_rep[NSTATES], is_rep_g0[NSTATES], is_rep_g1[NSTATES], is_rep_g2[NSTATES], is_rep0_long[NSTATES][16];
    Prob pos_slot[4][64], spec_pos[128], align[16];
    LenEnc len, rep_len;
    Prob lit[1<<LC][0x300];
    int state; uint32_t rep[4];             /* distances, 0-based (1 = the byte before last) */
} Enc;

static void probs_init(Prob *p, size_t n){ for(size_t i=0;i<n;i++) p[i]=kTotal/2; }
static void enc_init(Enc *e){
    memset(e,0,sizeof *e);
    probs_init(&e->is_match[0][0],sizeof e->is_match/sizeof(Prob)); probs_init(e->is_rep,NSTATES);
    probs_init(e->is_rep_g0,NSTATES); probs_init(e->is_rep_g1,NSTATES); probs_init(e->is_rep_g2,NSTATES);
    probs_init(&e->is_rep0_long[0][0],sizeof e->is_rep0_long/sizeof(Prob));
    probs_init(&e->pos_slot[0][0],sizeof e->pos_slot/sizeof(Prob)); probs_init(e->spec_pos,128); probs_init(e->align,16);
    probs_init((Prob*)&e->len,sizeof e->len/sizeof(Prob)); probs_init((Prob*)&e->rep_len,sizeof e->rep_len/sizeof(Prob));
    probs_init(&e->lit[0][0],sizeof e->lit/sizeof(Prob));
    e->rc.range=0xFFFFFFFFu; e->rc.cache_size=1;
}
static void len_enc(RC *rc, LenEnc *l, uint32_t len, unsigned ps){
    len-=MINLEN;
    if(len<8){ rc_bit(rc,&l->choice,0); tree(rc,l->low[ps],3,len); }
    else if(len<16){ rc_bit(rc,&l->choice,1); rc_bit(rc,&l->choice2,0); tree(rc,l->mid[ps],3,len-8); }
    else { rc_bit(rc,&l->choice,1); rc_bit(rc,&l->choice2,1); tree(rc,l->high,8,len-16); }
}
static void literal(Enc *e, const uint8_t *d, size_t pos){
    unsigned ps=(unsigned)pos&(POSSTATES-1);
    rc_bit(&e->rc,&e->is_match[e->state][ps],0);
    Prob *p=e->lit[pos?d[pos-1]>>(8-LC):0];
    unsigned b=d[pos], sym=1;
    if(e->state>=7){                                       /* after a match: the byte at rep0 steers the first bits */
        unsigned mb=d[pos-e->rep[0]-1], same=1;
        for(int i=7;i>=0;i--){ unsigned bit=(b>>i)&1;
            if(same){ unsigned m=(mb>>i)&1; rc_bit(&e->rc,&p[0x100+(m<<8)+sym],bit); same=m==bit; }
            else rc_bit(&e->rc,&p[sym],bit);
            sym=(sym<<1)|bit; }
    } else for(int i=7;i>=0;i--){ unsigned bit=(b>>i)&1; rc_bit(&e->rc,&p[sym],bit); sym=(sym<<1)|bit; }
    e->state= e->state<4 ? 0 : e->state<10 ? e->state-3 : e->state-6;
}
static unsigned pos_slot_of(uint32_t dist){
    if(dist<4) return dist;
    unsigned n=31; while(!(dist>>n)) n--;
    return (n<<1)|((dist>>(n-1))&1);
}
static void match(Enc *e, size_t pos, uint32_t dist, uint32_t len){
    unsigned ps=(unsigned)pos&(POSSTATES-1);
    rc_bit(&e->rc,&e->is_match[e->state][ps],1);
    rc_bit(&e->rc,&e->is_rep[e->state],0);
    len_enc(&e->rc,&e->len,len,ps);
    unsigned slot=pos_slot_of(dist), lps=len-MINLEN<3?len-MINLEN:3;
    tree(&e->rc,e->pos_slot[lps],6,slot);
    if(slot>=4){
        unsigned footer=(slot>>1)-1; uint32_t base=(2|(slot&1))<<footer, reduced=dist-base;
        if(slot<14) tree_rev(&e->rc,e->spec_pos+base-slot-1,(int)footer,reduced);
        else { rc_direct(&e->rc,reduced>>4,(int)footer-4); tree_rev(&e->rc,e->align,4,reduced&15); }
    }
    e->rep[3]=e->rep[2]; e->rep[2]=e->rep[1]; e->rep[1]=e->rep[0]; e->rep[0]=dist;
    e->state= e->state<7 ? 7 : 10;
}
static void rep_match(Enc *e, size_t pos, int which, uint32_t len){   /* len 1: a short rep (rep0) */
    unsigned ps=(unsigned)pos&(POSSTATES-1);
    rc_bit(&e->rc,&e->is_match[e->state][ps],1);
    rc_bit(&e->rc,&e->is_rep[e->state],1);
    if(which==0){
        rc_bit(&e->rc,&e->is_rep_g0[e->state],0);
        rc_bit(&e->rc,&e->is_rep0_long[e->state][ps],len==1?0:1);
        if(len==1){ e->state= e->state<7 ? 9 : 11; return; }
    } else {
        uint32_t d=e->rep[which];
        rc_bit(&e->rc,&e->is_rep_g0[e->state],1);
        if(which==1) rc_bit(&e->rc,&e->is_rep_g1[e->state],0);
        else { rc_bit(&e->rc,&e->is_rep_g1[e->state],1); rc_bit(&e->rc,&e->is_rep_g2[e->state],which==3);
               if(which==3) e->rep[3]=e->rep[2]; e->rep[2]=e->rep[1]; }
        e->rep[1]=e->rep[0]; e->rep[0]=d;
    }
    len_enc(&e->rc,&e->rep_len,len,ps);
    e->state= e->state<7 ? 8 : 11;
}

/* ---------------------------------------------------------------- match finding */

enum { HBITS=16, CHAIN=192, NICE=96 };
typedef struct { const uint8_t *d; size_t n; int32_t *head, *prev; } MF;
static unsigned h3(const uint8_t *p){ return (unsigned)((p[0]<<8 ^ p[1]<<4 ^ p[2])*2654435761u)>>(32-HBITS); }
static void mf_insert(MF *m, size_t pos){
    if(pos+3>m->n) return;
    unsigned h=h3(m->d+pos); m->prev[pos]=m->head[h]; m->head[h]=(int32_t)pos;
}
static uint32_t match_len(const uint8_t *d, size_t n, size_t pos, size_t from){
    uint32_t l=0, max=(uint32_t)(n-pos<MAXLEN?n-pos:MAXLEN);
    while(l<max && d[from+l]==d[pos+l]) l++;
    return l;
}
/* the longest match at pos among earlier positions (not inserted yet): its length, *dist 0-based */
static uint32_t mf_find(MF *m, size_t pos, uint32_t *dist){
    uint32_t best=0;
    if(pos+3>m->n) return 0;
    int32_t c=m->head[h3(m->d+pos)];
    for(int k=0;c>=0 && k<CHAIN;k++,c=m->prev[c]){
        uint32_t l=match_len(m->d,m->n,pos,(size_t)c);
        if(l>best || (l==best && l && pos-(size_t)c-1<*dist)){ best=l; *dist=(uint32_t)(pos-(size_t)c-1); if(l>=NICE) break; }
    }
    return best;
}

/* a match worth taking: short ones only near */
static int worth(uint32_t len, uint32_t dist){ return len>=3 ? (len>3 || dist<(1u<<14)) : len==2 && dist<128; }

static void lzma_encode(const uint8_t *d, size_t n, RC *out){
    Enc *e=(Enc*)xmalloc(sizeof(Enc)); enc_init(e);
    MF m; m.d=d; m.n=n; m.head=(int32_t*)xmalloc(sizeof(int32_t)<<HBITS); m.prev=(int32_t*)xmalloc(sizeof(int32_t)*(n+1));
    for(size_t i=0;i<((size_t)1<<HBITS);i++) m.head[i]=-1;
    size_t pos=0;
    uint32_t next_len=0, next_dist=0; int have_next=0;
    while(pos<n){
        uint32_t dist=0, len=have_next?next_len:mf_find(&m,pos,&dist);
        if(have_next) dist=next_dist;
        have_next=0;
        /* the last four distances */
        uint32_t rlen=0; int rwhich=-1;
        for(int r=0;r<4;r++){ if(e->rep[r]+1>pos) continue;
            uint32_t l=match_len(d,n,pos,pos-e->rep[r]-1); if(l>rlen){ rlen=l; rwhich=r; } }
        if(rlen>=2 && (rlen+1>=len || (rlen+2>=len && dist>=(1u<<9)) || (rlen+3>=len && dist>=(1u<<15)))){
            rep_match(e,pos,rwhich,rlen);
            for(uint32_t k=0;k<rlen;k++) mf_insert(&m,pos+k);
            pos+=rlen; continue;
        }
        if(worth(len,dist)){
            mf_insert(&m,pos);                               /* lazy: a longer one next? */
            uint32_t d2=0, l2=pos+1<n?mf_find(&m,pos+1,&d2):0;
            if(l2>len+1 || (l2==len+1 && !(d2>>7 > dist))){
                if(pos && e->rep[0]+1<=pos && d[pos]==d[pos-e->rep[0]-1] && e->state>=7) rep_match(e,pos,0,1);
                else literal(e,d,pos);
                pos++; next_len=l2; next_dist=d2; have_next=1; continue;
            }
            match(e,pos,dist,len);
            for(uint32_t k=1;k<len;k++) mf_insert(&m,pos+k);
            pos+=len; continue;
        }
        if(pos && e->rep[0]+1<=pos && d[pos]==d[pos-e->rep[0]-1]) rep_match(e,pos,0,1);   /* short rep */
        else literal(e,d,pos);
        mf_insert(&m,pos); pos++;
    }
    rc_flush(&e->rc);
    *out=e->rc;
    free(m.head); free(m.prev); free(e);
}

/* ---------------------------------------------------------------- calltrick */

/* calls / jumps to absolute big-endian with a marker; 0 if no marker byte is free */
static int calltrick(uint8_t *d, size_t n, uint32_t *count, uint8_t *marker){
    uint8_t used[256]; memset(used,0,sizeof used);
    size_t *at=(size_t*)xmalloc(sizeof(size_t)*(n/5+1)); size_t na=0;
    for(size_t i=0;i+5<=n;){
        if(d[i]!=0xE8 && d[i]!=0xE9){ i++; continue; }
        uint32_t rel=(uint32_t)d[i+1]|(uint32_t)d[i+2]<<8|(uint32_t)d[i+3]<<16|(uint32_t)d[i+4]<<24;
        uint32_t abs=rel+(uint32_t)(i+5);
        if(abs<n && abs<0x1000000u){
            d[i+1]=0; d[i+2]=(uint8_t)(abs>>16); d[i+3]=(uint8_t)(abs>>8); d[i+4]=(uint8_t)abs;
            at[na++]=i+1; i+=5;
        } else { used[d[i+1]]=1; i++; }
    }
    int m=-1; for(int k=0;k<256;k++) if(!used[k]){ m=k; break; }
    if(m<0){ free(at); return 0; }
    for(size_t k=0;k<na;k++) d[at[k]]=(uint8_t)m;
    free(at);
    *count=(uint32_t)na; *marker=(uint8_t)m;
    return 1;
}

/* ---------------------------------------------------------------- the file */

static uint8_t *kpck(const uint8_t *d, size_t n, int method, uint32_t ctn, uint8_t cti, size_t *outn){
    RC rc; lzma_encode(d,n,&rc);
    size_t body=rc.n-1, tail=(method&0x40)?5:0;              /* (the range coder's first byte: 0) */
    uint8_t *o=(uint8_t*)xmalloc(12+body+tail);
    memcpy(o,"KPCK",4);
    o[4]=(uint8_t)n; o[5]=(uint8_t)(n>>8); o[6]=(uint8_t)(n>>16); o[7]=(uint8_t)(n>>24);
    o[8]=(uint8_t)method; o[9]=o[10]=o[11]=0;
    memcpy(o+12,rc.out+1,body);
    if(body>=4){ uint8_t t; t=o[12]; o[12]=o[15]; o[15]=t; t=o[13]; o[13]=o[14]; o[14]=t; }   /* the first code dword: little-endian */
    if(tail){ uint8_t *t=o+12+body; t[0]=(uint8_t)ctn; t[1]=(uint8_t)(ctn>>8); t[2]=(uint8_t)(ctn>>16); t[3]=(uint8_t)(ctn>>24); t[4]=cti; }
    free(rc.out);
    *outn=12+body+tail;
    return o;
}

int aot_kpack(const unsigned char *in, size_t n, unsigned char **out, size_t *outn){
    if(n<16) return 1;
    size_t n1; uint8_t *best=kpck(in,n,1,0,0,&n1);
    uint8_t *ct=(uint8_t*)xmalloc(n); memcpy(ct,in,n);
    uint32_t ctn; uint8_t cti;
    if(calltrick(ct,n,&ctn,&cti)){
        size_t n2; uint8_t *b2=kpck(ct,n,1|0x40,ctn,cti,&n2);
        if(n2<n1){ free(best); best=b2; n1=n2; } else free(b2);
    }
    free(ct);
    if(n1>=n){ free(best); return 1; }                       /* not smaller: as it is */
    *out=best; *outn=n1;
    return 0;
}
