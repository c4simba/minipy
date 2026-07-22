/* ========================= Interned strings (qstr) ========================= */

#include "qstr.h"

/* Open-addressing hash set of canonical string pointers. */
static const char **q_pool = NULL;   /* all interned strings */
static int q_count = 0, q_cap = 0;
static int *q_buckets = NULL;        /* bucket -> (pool index + 1); 0 = empty */
static int q_nbuckets = 0;

static unsigned q_hash(const char *s, int n){
    unsigned h = 2166136261u;                 /* FNV-1a */
    for(int i=0;i<n;i++){ h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static void q_rehash(int newn){
    int *nb = MPY_NEW_ARR(int, newn);
    for(int i=0;i<newn;i++) nb[i]=0;
    for(int i=0;i<q_count;i++){
        const char *s=q_pool[i];
        unsigned h=q_hash(s,(int)strlen(s)) & (unsigned)(newn-1);
        while(nb[h]) h=(h+1)&(unsigned)(newn-1);
        nb[h]=i+1;
    }
    free(q_buckets); q_buckets=nb; q_nbuckets=newn;
}

const char *qstr_intern_n(const char *s, int n){
    if(q_nbuckets==0) q_rehash(64);
    unsigned h = q_hash(s,n) & (unsigned)(q_nbuckets-1);
    while(q_buckets[h]){
        const char *cand = q_pool[q_buckets[h]-1];
        if((int)strlen(cand)==n && memcmp(cand,s,(size_t)n)==0) return cand;
        h=(h+1)&(unsigned)(q_nbuckets-1);
    }
    /* not found: store a fresh permanent copy */
    char *copy = xstrndup2(s,n);
    if(q_count==q_cap){ q_cap=q_cap?q_cap*2:128; q_pool=(const char**)xrealloc(q_pool,sizeof(char*)*(size_t)q_cap); }
    q_pool[q_count]=copy;
    q_buckets[h]=q_count+1;
    q_count++;
    if(q_count*4 >= q_nbuckets*3) q_rehash(q_nbuckets*2);   /* keep load < 0.75 */
    return copy;
}

const char *qstr_intern(const char *s){ return qstr_intern_n(s,(int)strlen(s)); }
